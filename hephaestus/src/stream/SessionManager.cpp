#include "SessionManager.h"
#include "ChannelViewerRegistry.h"
#include "../log/RuntimeFlags.h"
#include <iostream>
#include <chrono>

// How often the cached channel list / stream buffer size are refreshed from
// Kairos in the background. getOrCreate() used to call kairos.getChannels()
// and kairos.getBufferSize() synchronously on every new session — each
// subject to httplib's full 5 s connect / 10 s read timeout — while holding
// `mtx`, so a single slow/unreachable Kairos response stalled *every*
// concurrent tuning attempt (not just the one that triggered it) for up to
// 10+ seconds. Roku/Plex give up long before that, so the very next attempt
// (once Kairos recovers) looks like "it just needed a retry." Reading from a
// periodically-refreshed cache means routine session creation never makes a
// blocking network call at all.
//
// Two cadences: 15s while a session is active, 5min while idle — these are
// admin-edited debug flags/channel defs, not anything that needs sub-minute
// freshness with nobody watching.
static constexpr auto kActiveRefreshInterval = std::chrono::seconds(15);
static constexpr auto kIdleRefreshInterval   = std::chrono::minutes(5);

SessionManager::SessionManager(KairosClient& kairos, std::string ffmpeg_path, StreamOptions opts)
	: kairos(kairos)
	, ffmpeg_path(std::move(ffmpeg_path))
	, stream_opts(std::move(opts))
{
	refreshCache(); // blocking, but only once, at startup
	refresh_thread = std::thread([this]
	{
		while (!stop_refresh.load())
		{
			bool active;
			{
				std::lock_guard<std::mutex> lock(mtx);
				active = !sessions.empty();
			}
			auto wait = active ? kActiveRefreshInterval : kIdleRefreshInterval;
			for (auto elapsed = std::chrono::seconds(0); elapsed < wait && !stop_refresh.load();
				 elapsed      += std::chrono::seconds(1))
				std::this_thread::sleep_for(std::chrono::seconds(1));
			if (stop_refresh.load()) break;
			refreshCache();
		}
	});
}

SessionManager::~SessionManager()
{
	stop_refresh.store(true);
	if (refresh_thread.joinable()) refresh_thread.join();
}

void SessionManager::refreshCache()
{
	auto channels = kairos.getChannels();
	auto bs       = kairos.getBufferSize();
	auto verbose  = kairos.getVerboseTranscodeLogs();
	std::lock_guard<std::mutex> lock(cache_mtx);
	// An empty fetch almost always means Kairos was unreachable, not that every
	// channel was deleted — keep the last-known-good list rather than blanking it.
	if (!channels.empty()) cached_channels = std::move(channels);
	if (bs) cached_buffer_size = *bs * 1024; // KB -> bytes
	if (verbose)
	{
		cached_verbose_transcode_logs = verbose;
		// Also feeds LogBuffer::push()'s [ffmpeg]/[sessions] gating — see
		// log/RuntimeFlags.h. SessionManager is the only one of the three
		// *SessionManager classes always constructed, so it owns this refresh.
		g_verbose_transcode_logs.store(*verbose, std::memory_order_relaxed);
	}
}

std::shared_ptr<ChannelSession> SessionManager::getOrCreate(const std::string& channelId, const std::string& bucket)
{
	auto key = std::make_pair(channelId, bucket);
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = sessions.find(key);
		if (it != sessions.end() && it->second->isActive()) return it->second;
	}

	// Apply per-channel language overrides and transcode quality from the cached
	// Kairos channel config. A channel not found in this list is invalid/unknown —
	// since start() now always succeeds (it falls back to a splash), this existence
	// check is what actually rejects bogus channel IDs instead of silently serving
	// a logo forever.
	StreamOptions opts = stream_opts;
	bool found         = false;
	{
		std::lock_guard<std::mutex> lock(cache_mtx);
		for (const auto& ch : cached_channels)
		{
			if (ch.channel_id == channelId)
			{
				found = true;
				if (!ch.audio_lang.empty()) opts.audio_lang = ch.audio_lang;
				if (!ch.subtitle_lang.empty()) opts.subtitle_lang = ch.subtitle_lang;
				opts.max_resolution     = ch.stream_resolution;
				opts.video_bitrate_kbps = ch.stream_video_bitrate;
				opts.audio_bitrate_kbps = ch.stream_audio_bitrate > 0 ? ch.stream_audio_bitrate : 192;
				opts.logo_path          = ch.logo_path;
				break;
			}
		}
		if (cached_buffer_size > 0) opts.buffer_size = cached_buffer_size;
		if (cached_verbose_transcode_logs) opts.verbose_transcode_logs = *cached_verbose_transcode_logs;
	}
	if (!found)
	{
		std::cerr << "[sessions] unknown channel " << channelId << "\n";
		return nullptr;
	}

	std::lock_guard<std::mutex> lock(mtx);
	// Re-check: another thread may have created the session while we were
	// reading the cache above (which intentionally isn't held under `mtx`).
	auto it = sessions.find(key);
	if (it != sessions.end() && it->second->isActive()) return it->second;

	// Create and start a new session
	auto session = std::make_shared<ChannelSession>(channelId, kairos, ffmpeg_path, opts, bucket);
	if (viewer_registry_)
	{
		ChannelViewerRegistry* registry = viewer_registry_;
		session->onItemPlaying          = [registry](const std::string& cid, const std::optional<MediaInfo>& info, int audioTrack)
		{
			registry->reassignForChannel(cid, info, audioTrack);
		};
	}
	if (!session->start())
	{
		std::cerr << "[sessions] failed to start session for channel " << channelId << " bucket " << bucket << "\n";
		return nullptr;
	}

	sessions[key] = session;
	return session;
}

void SessionManager::reap()
{
	std::lock_guard<std::mutex> lock(mtx);
	for (auto it = sessions.begin(); it != sessions.end();)
	{
		if (!it->second->isActive()) it = sessions.erase(it);
		else ++it;
	}
}

std::vector<std::shared_ptr<ChannelSession>> SessionManager::listActive()
{
	std::lock_guard<std::mutex> lock(mtx);
	std::vector<std::shared_ptr<ChannelSession>> out;
	for (auto& [id, session] : sessions) if (session->isActive()) out.push_back(session);
	return out;
}