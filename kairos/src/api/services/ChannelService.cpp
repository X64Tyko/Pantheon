#include "ChannelService.h"
#include "../RouteHelpers.h"
#include "../ScheduleCache.h"
#include "../../conf/ConfStore.h"
#include "../../db/ChannelRepository.h"
#include "../../db/ChannelSerializer.h"
#include "../../log/LogBuffer.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

static bool isValidTimezone(const std::string& tz) {
	if (tz.empty()) return false;
	try { std::chrono::locate_zone(tz); return true; } catch (...) { return false; }
}

ChannelService::ChannelService(const ServiceContext& ctx)
	: db_(ctx.db), conf_(ctx.conf), schedule_cache_(ctx.schedule_cache), logs_(ctx.logs) {}

void ChannelService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/channels", [this](const Req&, Res& res) {
		try {
			auto channels = ChannelRepository(db_).listChannels();
			json result = json::array();
			for (const auto& c : channels) {
				json channel = {
					{"channel_id",               c.channel_id},
					{"name",                     c.name},
					{"number",                   c.number},
					{"timezone",                 c.timezone},
					{"default_filler_selection", c.default_filler_selection},
					{"seed",                     c.seed},
					{"advance_mode",             c.advance_mode},
					{"offline_video_path",       c.offline_video_path},
					{"offline_image_path",       c.offline_image_path},
					{"offline_audio_id",         c.offline_audio_id},
					{"offline_audio_type",       c.offline_audio_type},
					{"offline_audio_title",      c.offline_audio_title},
					{"logo_path",                c.logo_path},
					{"audio_lang",               c.audio_lang},
					{"subtitle_lang",            c.subtitle_lang},
					{"stream_resolution",        c.stream_resolution},
					{"stream_video_bitrate",     c.stream_video_bitrate},
					{"stream_audio_bitrate",     c.stream_audio_bitrate},
				};
				if (!c.anchor_hashes.empty()) {
					try { channel["anchor_hashes"] = json::parse(c.anchor_hashes); } catch (...) {}
				}
				channel["default_filler_entries"] = ChannelRepository(db_).listFillerEntries(c.channel_id);
				result.push_back(channel);
			}
			route::ok(res, result.dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/channels", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/channels", [this](const Req& req, Res& res) {
		try {
			auto b = json::parse(req.body);
			std::string name         = b.value("name", "");
			int         number       = b.value("number", 0);
			std::string timezone     = b.value("timezone", "UTC");
			std::string advance_mode = b.value("advance_mode", "scheduled");
			if (name.empty() || number == 0) {
				route::err(res, 400, "name and number required"); return;
			}
			if (!isValidTimezone(timezone)) {
				route::err(res, 400, "invalid timezone: " + timezone); return;
			}
			std::string channel_id = ChannelRepository(db_).create(name, number, timezone, advance_mode);
			res.status = 201;
			route::ok(res, json{{"channel_id", channel_id}}.dump());
		} catch (const SQLite::Exception& e) {
			route::logErr("POST /api/channels", e);
			route::err(res, 409, e.what());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("POST /api/channels", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Patch("/api/channels/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			ChannelRepository repo(db_);
			auto upd  = [&](const char* col, const std::string& val) { repo.updateField(id, col, val); };
			auto updI = [&](const char* col, int val)                { repo.updateField(id, col, val); };
			if (b.contains("name"))                     upd("name",                     b["name"]);
			if (b.contains("timezone")) {
				std::string tz = b["timezone"].get<std::string>();
				if (!isValidTimezone(tz)) { route::err(res, 400, "invalid timezone: " + tz); return; }
				upd("timezone", tz);
			}
			if (b.contains("number"))                   updI("number",                   b["number"].get<int>());
			if (b.contains("default_filler_selection")) upd("default_filler_selection", b["default_filler_selection"]);
			if (b.contains("advance_mode"))             upd("advance_mode",             b["advance_mode"]);
			if (b.contains("offline_video_path"))       upd("offline_video_path",       b["offline_video_path"]);
			if (b.contains("offline_image_path"))       upd("offline_image_path",       b["offline_image_path"]);
			if (b.contains("offline_audio_id"))         upd("offline_audio_id",         b["offline_audio_id"]);
			if (b.contains("offline_audio_type"))       upd("offline_audio_type",       b["offline_audio_type"]);
			if (b.contains("offline_audio_title"))      upd("offline_audio_title",      b["offline_audio_title"]);
			if (b.contains("logo_path"))                upd("logo_path",                b["logo_path"]);
			if (b.contains("audio_lang"))               upd("audio_lang",               b["audio_lang"]);
			if (b.contains("subtitle_lang"))            upd("subtitle_lang",            b["subtitle_lang"]);
			if (b.contains("stream_resolution")) {
				std::string resolution = b["stream_resolution"].get<std::string>();
				if (resolution != "source" && resolution != "1080p" && resolution != "720p" && resolution != "480p") {
					route::err(res, 400, "stream_resolution must be source|1080p|720p|480p"); return;
				}
				upd("stream_resolution", resolution);
			}
			if (b.contains("stream_video_bitrate"))     updI("stream_video_bitrate",    b["stream_video_bitrate"].get<int>());
			if (b.contains("stream_audio_bitrate"))     updI("stream_audio_bitrate",    b["stream_audio_bitrate"].get<int>());
			if (b.contains("seed"))                     updI("seed",                    b["seed"].get<int>());
			if (b.contains("anchor_hashes")) {
				std::string ah = b["anchor_hashes"].is_string()
					? b["anchor_hashes"].get<std::string>()
					: b["anchor_hashes"].dump();
				upd("anchor_hashes", ah);
			}
			if (b.contains("timezone") || b.contains("seed") || b.contains("advance_mode"))
				schedule_cache_.clear(id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/channels/" + id, e);
			route::err(res, 400, e.what());
		}
	});

	svr.Delete("/api/channels/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			ChannelRepository(db_).remove(id);
			logs_.push("[api] deleted channel: " + id);
			route::ok(res, json{{"deleted", id}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/channels/" + id, e);
			route::err(res, 500, e.what());
		}
	});

	// logo_path is a free-text admin field (see ChannelDefaultsPanel.tsx) —
	// in practice either a local filesystem path or a remote image URL an
	// admin pasted in directly. Local paths get served as bytes (mirroring
	// the movie/show/episode thumb pattern); remote URLs get redirected
	// rather than proxied, since ContentService's fetch-and-cache proxyImage()
	// is scoped to that service and this doesn't need its CDN-hotlink-bypass
	// logic — plain image hosts and XMLTV consumers both follow redirects fine.
	svr.Get("/api/channels/:id/logo", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		auto channel = ChannelRepository(db_).findById(id);
		if (!channel || channel->logo_path.empty()) { res.status = 404; return; }
		if (channel->logo_path.rfind("http://", 0) == 0 || channel->logo_path.rfind("https://", 0) == 0) {
			res.set_redirect(channel->logo_path);
			return;
		}
		auto path = conf_.applyPathMap(channel->logo_path);
		if (!std::filesystem::exists(path)) { res.status = 404; return; }
		res.set_header("Cache-Control", "public, max-age=3600");
		res.set_file_content(path);
	});

	svr.Get("/api/channels/:id/export", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		bool deep = (req.has_param("depth") && req.get_param_value("depth") == "deep");
		try {
			route::ok(res, ChannelSerializer(db_).exportChannel(channel_id, deep).dump(2));
		} catch (const std::exception& e) {
			route::logErr("GET /api/channels/:id/export", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/channels/import/preview", [this](const Req& req, Res& res) {
		try {
			auto body = json::parse(req.body);
			bool deep = (body.value("depth", "shallow") == "deep");
			route::ok(res, ChannelSerializer(db_).previewImport(body, deep).dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/channels/import/preview", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Post("/api/channels/import", [this](const Req& req, Res& res) {
		try {
			auto body = json::parse(req.body);
			bool deep = (body.value("depth", "shallow") == "deep");
			auto [channel_id, unresolved] = ChannelSerializer(db_).importChannel(body, deep);
			res.status = 201;
			logs_.push("[api] imported channel: " + channel_id);
			route::ok(res, json{{"channel_id", channel_id}, {"unresolved", unresolved}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/channels/import", e);
			route::err(res, 400, e.what());
		}
	});

	// ── Channel filler entry CRUD ─────────────────────────────────────────────

	svr.Post("/api/channels/:id/filler", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::string content_type = b.value("content_type", "filler_list");
			std::string content_id   = b.value("content_id",   "");
			std::string advancement  = b.value("advancement",  "sequential");
			int         weight       = b.value("weight",       1);
			if (content_id.empty()) { route::err(res, 400, "content_id required"); return; }
			std::optional<int> season_filter;
			if (b.contains("season_filter") && !b["season_filter"].is_null())
				season_filter = b["season_filter"].get<int>();
			auto fr = ChannelRepository(db_).addFillerEntry(
				channel_id, content_type, content_id, advancement, weight, season_filter);
			json resp = {
				{"id",           fr.id},
				{"content_type", content_type},
				{"content_id",   content_id},
				{"title",        fr.title},
				{"advancement",  advancement},
				{"weight",       weight},
				{"position",     fr.position},
			};
			if (season_filter.has_value()) resp["season_filter"] = season_filter.value();
			res.status = 201;
			route::ok(res, resp.dump());
		} catch (const SQLite::Exception& e) {
			route::logErr("POST /api/channels/:id/filler", e);
			route::err(res, 409, e.what());
		} catch (const std::exception& e) {
			route::logErr("POST /api/channels/:id/filler", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Patch("/api/channels/:id/filler/:eid", [this](const Req& req, Res& res) {
		auto eid = std::stoi(req.path_params.at("eid"));
		try {
			auto b = json::parse(req.body);
			ChannelRepository repo(db_);
			if (b.contains("advancement"))
				repo.updateFillerEntryField(eid, "advancement", b["advancement"].get<std::string>());
			if (b.contains("weight"))
				repo.updateFillerEntryField(eid, "weight", b["weight"].get<int>());
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/channels/:id/filler/:eid", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Delete("/api/channels/:id/filler/:eid", [this](const Req& req, Res& res) {
		auto eid = std::stoi(req.path_params.at("eid"));
		try {
			ChannelRepository(db_).removeFillerEntry(eid);
			route::ok(res, json{{"deleted", eid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/channels/:id/filler/" + std::to_string(eid), e);
			route::err(res, 500, e.what());
		}
	});
}
