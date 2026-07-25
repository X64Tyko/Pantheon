#include "KairosClient.h"
#include "InternalToken.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

KairosClient::KairosClient(std::string base_url, std::string internal_token_conf_path)
	: base_url(std::move(base_url))
	, internal_token_conf_path(std::move(internal_token_conf_path))
{
}

static httplib::Client makeClient(const std::string& url)
{
	httplib::Client c(url);
	c.set_connection_timeout(5);
	c.set_read_timeout(10);
	return c;
}

std::optional<KairosNowResponse> KairosClient::getNow(const std::string& channelId, int64_t atMs)
{
	auto cli         = makeClient(base_url);
	std::string path = "/api/channels/" + channelId + "/now";
	if (atMs >= 0) path += "?at=" + std::to_string(atMs);

	auto res = cli.Get(path);
	if (!res || res->status != 200)
	{
		std::cerr << "[kairos] GET " << path << " -> "
			<< (res ? std::to_string(res->status) : "no response") << "\n";
		return std::nullopt;
	}

	try
	{
		auto j = json::parse(res->body);
		KairosNowResponse r;
		r.item_type           = j.value("item_type", "");
		r.item_id             = j.value("item_id", "");
		r.file_path           = j.value("file_path", "");
		r.title               = j.value("title", "");
		r.block_id            = j.value("block_id", "");
		r.duration_ms         = j.value("duration_ms", int64_t(0));
		r.wall_clock_start_ms = j.value("wall_clock_start_ms", int64_t(0));
		r.wall_clock_end_ms   = j.value("wall_clock_end_ms", int64_t(0));
		r.is_filler           = j.value("is_filler", false);

		auto optStr = [&](const char* k) -> std::optional<std::string>
		{
			if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
			return std::nullopt;
		};
		r.show_title         = optStr("show_title");
		r.show_id            = optStr("show_id");
		r.source_id          = optStr("source_id");
		r.external_id        = optStr("external_id");
		r.offline_image_path = optStr("offline_image_path");
		r.offline_audio_path = optStr("offline_audio_path");

		if (j.contains("season") && j["season"].is_number()) r.season = j["season"].get<int>();
		if (j.contains("episode_num") && j["episode_num"].is_number()) r.episode_num = j["episode_num"].get<int>();
		return r;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[kairos] getNow JSON parse error: " << e.what() << "\n";
		return std::nullopt;
	}
}

void KairosClient::markPlayed(const std::string& channelId,
							  const std::string& itemType,
							  const std::string& itemId,
							  const std::string& blockId,
							  int64_t durationActualMs)
{
	auto cli  = makeClient(base_url);
	json body = {
		{"item_type", itemType},
		{"item_id", itemId},
		{"block_id", blockId},
		{"duration_actual_ms", durationActualMs}
	};
	httplib::Headers headers;
	// Re-read each call rather than cache — cheap, and picks up a rotated
	// token immediately with no restart.
	std::string token = readKairosInternalToken(internal_token_conf_path);
	if (!token.empty()) headers.emplace("X-Internal-Token", token);
	auto res = cli.Post("/api/channels/" + channelId + "/played",
						headers, body.dump(), "application/json");
	if (!res || res->status / 100 != 2)
	{
		std::cerr << "[kairos] POST /played failed for channel " << channelId
			<< ": " << (res ? std::to_string(res->status) : "no response") << "\n";
	}
}

std::vector<KairosChannel> KairosClient::getChannels()
{
	auto cli = makeClient(base_url);
	auto res = cli.Get("/api/channels");
	if (!res || res->status != 200)
	{
		std::cerr << "[kairos] GET /api/channels -> "
			<< (res ? std::to_string(res->status) : "no response") << "\n";
		return {};
	}
	try
	{
		auto j = json::parse(res->body);
		std::vector<KairosChannel> channels;
		for (auto& item : j)
		{
			KairosChannel ch;
			ch.channel_id           = item.value("channel_id", "");
			ch.name                 = item.value("name", "");
			ch.number               = item.value("number", 0);
			ch.audio_lang           = item.value("audio_lang", "");
			ch.subtitle_lang        = item.value("subtitle_lang", "");
			ch.logo_path            = item.value("logo_path", "");
			ch.stream_resolution    = item.value("stream_resolution", "source");
			ch.stream_video_bitrate = item.value("stream_video_bitrate", 0);
			ch.stream_audio_bitrate = item.value("stream_audio_bitrate", 192);
			if (!ch.channel_id.empty()) channels.push_back(std::move(ch));
		}
		return channels;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[kairos] getChannels JSON parse error: " << e.what() << "\n";
		return {};
	}
}

std::optional<PlaybackInfo> KairosClient::getPlaybackInfo(const std::string& contentType,
														  const std::string& contentId,
														  const std::string& bearerToken)
{
	auto cli         = makeClient(base_url);
	std::string path = "/api/playback/" + contentType + "/" + contentId;
	httplib::Headers headers;
	if (!bearerToken.empty()) headers.emplace("Authorization", "Bearer " + bearerToken);
	auto res = cli.Get(path, headers);
	if (!res || res->status != 200)
	{
		std::cerr << "[kairos] GET " << path << " -> "
			<< (res ? std::to_string(res->status) : "no response") << "\n";
		return std::nullopt;
	}
	try
	{
		auto j = json::parse(res->body);
		PlaybackInfo info;
		info.file_path               = j.value("file_path", "");
		info.title                   = j.value("title", "");
		info.duration_ms             = j.value("duration_ms", int64_t(0));
		info.preferred_audio_lang    = j.value("preferred_audio_lang", "");
		info.preferred_subtitle_lang = j.value("preferred_subtitle_lang", "");
		if (j.contains("external_subtitles") && j["external_subtitles"].is_array())
		{
			for (const auto& s : j["external_subtitles"])
			{
				ExternalSubtitle sub;
				sub.file_path = s.value("file_path", "");
				sub.language  = s.value("language", "");
				sub.forced    = s.value("forced", false);
				sub.sdh       = s.value("sdh", false);
				sub.title     = s.value("title", "");
				if (!sub.file_path.empty()) info.external_subtitles.push_back(std::move(sub));
			}
		}
		return info;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[kairos] getPlaybackInfo JSON parse error: " << e.what() << "\n";
		return std::nullopt;
	}
}

std::optional<int> KairosClient::getBufferSize()
{
	auto cli = makeClient(base_url);
	auto res = cli.Get("/api/config/public-settings");
	if (!res || res->status != 200)
	{
		std::cerr << "[kairos] GET /api/config/public-settings -> "
			<< (res ? std::to_string(res->status) : "no response") << "\n";
		return std::nullopt;
	}
	try
	{
		auto j = json::parse(res->body);
		if (j.contains("stream_buffer_size") && j["stream_buffer_size"].is_number_integer())
		{
			return j["stream_buffer_size"].get<int>();
		}
		return std::nullopt;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[kairos] getBufferSize JSON parse error: " << e.what() << "\n";
		return std::nullopt;
	}
}

std::optional<bool> KairosClient::getVerboseTranscodeLogs()
{
	auto cli = makeClient(base_url);
	auto res = cli.Get("/api/config/public-settings");
	if (!res || res->status != 200)
	{
		std::cerr << "[kairos] GET /api/config/public-settings -> "
			<< (res ? std::to_string(res->status) : "no response") << "\n";
		return std::nullopt;
	}
	try
	{
		auto j = json::parse(res->body);
		if (j.contains("verbose_transcode_logs") && j["verbose_transcode_logs"].is_boolean())
		{
			return j["verbose_transcode_logs"].get<bool>();
		}
		return std::nullopt;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[kairos] getVerboseTranscodeLogs JSON parse error: " << e.what() << "\n";
		return std::nullopt;
	}
}