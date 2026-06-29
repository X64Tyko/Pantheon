#include "SchedulerService.h"
#include "../RouteHelpers.h"
#include "../ScheduleCache.h"
#include "../ServiceContext.h"
#include "../../conf/ConfStore.h"
#include "../../db/ChannelRepository.h"
#include "../../db/ScheduleRepository.h"
#include "../../db/SourceRepository.h"
#include "../../scheduler/EPGMaterializer.h"
#include "../../scheduler/RuleEngine.h"
#include <nlohmann/json.hpp>
#include <ctime>
#include <map>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

SchedulerService::SchedulerService(const ServiceContext& ctx)
	: db_(ctx.db), conf_(ctx.conf), engine_(ctx.engine),
	  materializer_(ctx.materializer), schedule_cache_(ctx.schedule_cache)
{}

void SchedulerService::registerRoutes(httplib::Server& svr) {

	svr.Get("/playlist.m3u", [this](const Req& req, Res& res) {
		std::string host = req.get_header_value("Host");
		if (host.empty()) host = "localhost:8080";
		res.set_content(materializer_.generateM3U("http://" + host), "application/x-mpegURL");
	});

	auto xmltvHandler = [this](const Req& req, Res& res) {
		int hours = 24;
		if (req.has_param("hours")) {
			try { hours = std::stoi(req.get_param_value("hours")); } catch (...) {}
		}
		hours = std::max(1, std::min(hours, 72));
		res.set_content(materializer_.generateXMLTV(hours), "application/xml");
	};
	svr.Get("/epg.xml",          xmltvHandler);
	svr.Get("/api/epg.xml",      xmltvHandler);
	svr.Get("/api/xmltv.xml",    xmltvHandler);
	svr.Get("/api/channels.xml", xmltvHandler);

	// ── What's playing now ────────────────────────────────────────────────────
	svr.Get(R"(/api/channels/([^/]+)/now)", [this](const Req& req, Res& res) {
	  try {
		std::string channel_id = req.matches[1];
		auto t = std::time(nullptr);
		if (req.has_param("at")) {
			try {
				int64_t at_ms = std::stoll(req.get_param_value("at"));
				t = static_cast<std::time_t>(at_ms / 1000);
			} catch (...) {}
		}

		materializer_.ensureScheduled(channel_id, t - 3600, 4);

		ScheduleRepository sched(db_);
		if (auto row = sched.getNowProgram(channel_id, t)) {
			json j = {
				{"item_type",           row->item_type},
				{"item_id",             row->item_id},
				{"file_path",           conf_.applyPathMap(row->file_path)},
				{"duration_ms",         row->duration_ms},
				{"title",               row->title},
				{"block_id",            row->block_id},
				{"wall_clock_start_ms", row->wall_clock_start * 1000LL},
				{"wall_clock_end_ms",   row->wall_clock_end   * 1000LL},
				{"is_filler",           row->is_filler},
			};
			if (!row->show_title.empty()) {
				j["show_title"]  = row->show_title;
				j["show_id"]     = row->show_id;
				j["season"]      = row->season;
				j["episode_num"] = row->episode;
			}
			if (auto sm = SourceRepository(db_).getSourceMapping(row->item_id)) {
				j["source_id"]   = sm->source_id;
				j["external_id"] = sm->external_id;
			}
			route::ok(res, j.dump());
			return;
		}

		auto block_opt = engine_.resolveBlock(channel_id, t);
		std::optional<ScheduledItem> item_opt;
		if (block_opt)
			item_opt = engine_.nextItem(channel_id, *block_opt, std::time(nullptr));

		if (block_opt && item_opt) {
			const auto& item = *item_opt;
			json j = {
				{"item_type",            item.item_type},
				{"item_id",              item.item_id},
				{"file_path",            conf_.applyPathMap(item.file_path)},
				{"duration_ms",          item.duration_ms},
				{"title",                item.title},
				{"block_id",             item.block_id},
				{"wall_clock_start_ms",  static_cast<int64_t>(t) * 1000},
				{"wall_clock_end_ms",    static_cast<int64_t>(t) * 1000 + item.duration_ms},
				{"is_filler",            item.is_filler},
			};
			if (!item.show_title.empty()) {
				j["show_title"]  = item.show_title;
				j["show_id"]     = item.show_id;
				j["season"]      = item.season;
				j["episode_num"] = item.episode_num;
			}
			if (auto sm = SourceRepository(db_).getSourceMapping(item.item_id)) {
				j["source_id"]   = sm->source_id;
				j["external_id"] = sm->external_id;
			}
			route::ok(res, j.dump());
			return;
		}

		if (auto filler = sched.getChannelFillerFallback(channel_id)) {
			int64_t dur = filler->duration_ms;
			json j = {
				{"item_type",           filler->item_type},
				{"item_id",             filler->item_id},
				{"file_path",           conf_.applyPathMap(filler->file_path)},
				{"title",               filler->title},
				{"duration_ms",         dur},
				{"block_id",            ""},
				{"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
				{"wall_clock_end_ms",   static_cast<int64_t>(t) * 1000 + dur},
				{"is_filler",           true},
			};
			route::ok(res, j.dump());
			return;
		}

		if (auto offline = sched.getChannelOfflineConfig(channel_id)) {
			if (!offline->vid_path.empty()) {
				route::ok(res, json{
					{"item_type",           "offline"},
					{"file_path",           conf_.applyPathMap(offline->vid_path)},
					{"duration_ms",         0},
					{"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
				}.dump());
				return;
			}
			if (!offline->img_path.empty()) {
				json j = {
					{"item_type",           "offline"},
					{"offline_image_path",  conf_.applyPathMap(offline->img_path)},
					{"duration_ms",         0},
					{"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
				};
				if (!offline->audio_id.empty() && !offline->audio_typ.empty()) {
					if (auto ap = sched.getAudioFilePath(offline->audio_typ, offline->audio_id))
						j["offline_audio_path"] = conf_.applyPathMap(*ap);
				}
				route::ok(res, j.dump());
				return;
			}
		}

		route::err(res, 404, "no content or fallback configured for this channel");
	  } catch (const std::exception& e) {
		route::logErr("GET /api/channels/now", e); route::err(res, 500, e.what());
	  }
	});

	// ── What's playing next ───────────────────────────────────────────────────
	svr.Get(R"(/api/channels/([^/]+)/next)", [this](const Req& req, Res& res) {
	  try {
		std::string channel_id = req.matches[1];
		auto t = std::time(nullptr);

		materializer_.ensureScheduled(channel_id, t, 4);

		ScheduleRepository sched(db_);
		auto row = sched.getNextProgram(channel_id, t);
		if (!row) { route::err(res, 404, "no next item available"); return; }

		json j = {
			{"item_type",           row->item_type},
			{"item_id",             row->item_id},
			{"file_path",           conf_.applyPathMap(row->file_path)},
			{"duration_ms",         row->duration_ms},
			{"title",               row->title},
			{"block_id",            row->block_id},
			{"wall_clock_start_ms", row->wall_clock_start * 1000LL},
		};
		if (!row->show_title.empty()) {
			j["show_title"]  = row->show_title;
			j["show_id"]     = row->show_id;
			j["season"]      = row->season;
			j["episode_num"] = row->episode;
		}
		if (auto sm = SourceRepository(db_).getSourceMapping(row->item_id)) {
			j["source_id"]   = sm->source_id;
			j["external_id"] = sm->external_id;
		}
		route::ok(res, j.dump());
	  } catch (const std::exception& e) {
		route::logErr("GET /api/channels/next", e); route::err(res, 500, e.what());
	  }
	});

	// ── Report playback completion ────────────────────────────────────────────
	svr.Post(R"(/api/channels/([^/]+)/played)", [this](const Req& req, Res& res) {
		try {
			std::string channel_id = req.matches[1];
			auto b = json::parse(req.body);
			std::string item_type = b.value("item_type", "episode");
			std::string item_id   = b.value("item_id",   "");
			std::string block_id  = b.value("block_id",  "");
			int64_t duration_ms   = b.value("duration_actual_ms", int64_t(0));

			if (item_id.empty()) { route::err(res, 400, "item_id required"); return; }
			engine_.markPlayed(channel_id, block_id, item_type, item_id, duration_ms);
			materializer_.notifyPlayed(channel_id, item_id);
			if (ChannelRepository(db_).getAdvanceMode(channel_id) == "on_play")
				schedule_cache_.clear(channel_id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/channels/played", e);
			route::err(res, 400, e.what());
		}
	});

	// ── EPG preview ───────────────────────────────────────────────────────────
	svr.Post(R"(/api/channels/([^/]+)/epg/preview)", [this](const Req& req, Res& res) {
	  try {
		std::string channel_id = req.matches[1];

		json body = json::object();
		if (!req.body.empty()) {
			try { body = json::parse(req.body); } catch (...) {}
		}

		int hours = 336;
		if (body.contains("hours")) {
			try { hours = body["hours"].get<int>(); } catch (...) {}
		}
		hours = std::max(1, std::min(hours, 672));

		int  req_seed = -1;
		bool has_seed = body.contains("seed") && !body["seed"].is_null();
		if (has_seed) {
			try { req_seed = body["seed"].get<int>(); } catch (...) {}
		}

		bool has_blocks = body.contains("blocks") && body["blocks"].is_array()
		               && !body["blocks"].empty();

		auto now     = std::time(nullptr);
		auto horizon = static_cast<int64_t>(now + hours * 3600LL);

		std::time_t days_since_epoch = static_cast<std::time_t>(now / 86400);
		std::time_t day_of_week_mon0 = (days_since_epoch + 3) % 7;
		std::time_t week_anchor      = (days_since_epoch - day_of_week_mon0) * 86400;

		if (!has_blocks && !has_seed) {
			std::string cached;
			if (schedule_cache_.getPreview(channel_id, req_seed, week_anchor, cached)) {
				route::ok(res, cached);
				return;
			}
		}

		GenerateResult gr;
		if (has_blocks) {
			ScheduleRepository sched(db_);
			gr = sched.withPreviewBlocks(channel_id, body["blocks"],
				[&]() { return materializer_.generate(channel_id, now, hours, req_seed); });
		} else {
			gr = materializer_.generate(channel_id, now, hours, req_seed);
		}

		json arr = json::array();
		std::map<std::time_t, int> anchor_counts;

		for (const auto& item : gr.items) {
			std::time_t ws = item.wall_clock_start_ms / 1000;
			std::time_t we = item.wall_clock_end_ms   / 1000;
			if (we <= now)     continue;
			if (ws >= horizon) break;

			json j = {
				{"item_type",           item.item_type},
				{"item_id",             item.item_id},
				{"block_id",            item.block_id},
				{"wall_clock_start_ms", item.wall_clock_start_ms},
				{"wall_clock_end_ms",   item.wall_clock_end_ms},
				{"status",              "scheduled"},
				{"title",               item.title},
				{"duration_ms",         item.duration_ms},
			};
			if (!item.show_title.empty()) {
				j["show_title"]  = item.show_title;
				j["show_id"]     = item.show_id;
				j["season"]      = item.season;
				j["episode_num"] = item.episode_num;
			}
			arr.push_back(j);

			if (has_blocks && !item.is_filler) {
				std::time_t days = ws / 86400;
				std::time_t dow  = (days + 3) % 7;
				std::time_t anch = (days - dow) * 86400;
				anchor_counts[anch]++;
			}
		}

		json anchors_j = json::object();
		if (!has_blocks) {
			if (auto ah = ChannelRepository(db_).getAnchorHashes(channel_id)) {
				try { anchors_j = json::parse(*ah); } catch (...) {}
			}
			for (auto& [ts, snap_str] : gr.anchors) {
				try { anchors_j[std::to_string(ts)] = json::parse(snap_str); } catch (...) {}
			}
		} else {
			int cumulative = req_seed;
			for (auto& [anch, cnt] : anchor_counts) {
				cumulative += cnt;
				anchors_j[std::to_string(anch)] = cumulative;
			}
		}

		json divs_j = json::array();
		for (const auto& d : gr.divergences) {
			divs_j.push_back({
				{"wall_clock_start", d.wall_clock_start},
				{"wall_clock_end",   d.wall_clock_end},
				{"block_id",         d.block_id},
				{"prev_item_type",   d.prev_item_type},
				{"prev_item_id",     d.prev_item_id},
				{"new_item_type",    d.new_item_type},
				{"new_item_id",      d.new_item_id},
			});
		}

		std::string resp_body = json{
			{"programs",    arr},
			{"anchors",     anchors_j},
			{"divergences", divs_j}
		}.dump();
		if (!has_blocks && !has_seed)
			schedule_cache_.setPreview(channel_id, req_seed, week_anchor, resp_body);
		route::ok(res, resp_body);
	  } catch (const std::exception& e) {
		route::logErr("POST /api/channels/epg/preview", e); route::err(res, 500, e.what());
	  }
	});

	// ── Clear EPG cache ───────────────────────────────────────────────────────
	svr.Post(R"(/api/channels/([^/]+)/epg/clear)", [this](const Req& req, Res& res) {
	  try {
		schedule_cache_.clear(req.matches[1]);
		route::ok(res, json{{"ok", true}}.dump());
	  } catch (const std::exception& e) {
		route::logErr("POST /api/channels/epg/clear", e); route::err(res, 500, e.what());
	  }
	});

	// ── EPG projection ────────────────────────────────────────────────────────
	svr.Get(R"(/api/channels/([^/]+)/epg)", [this](const Req& req, Res& res) {
	  try {
		std::string channel_id = req.matches[1];
		int hours = 24;
		if (req.has_param("hours")) {
			try { hours = std::stoi(req.get_param_value("hours")); } catch (...) {}
		}
		hours = std::max(1, std::min(hours, 72));

		auto now = std::time(nullptr);
		materializer_.ensureScheduled(channel_id, now, hours);

		auto horizon = static_cast<int64_t>(now + hours * 3600LL);
		auto rows = ScheduleRepository(db_).getEpgPrograms(channel_id, now, horizon);

		json arr = json::array();
		for (const auto& r : rows) {
			json j = {
				{"item_type",           r.item_type},
				{"item_id",             r.item_id},
				{"block_id",            r.block_id},
				{"wall_clock_start_ms", r.wall_clock_start * 1000},
				{"wall_clock_end_ms",   r.wall_clock_end   * 1000},
				{"status",              r.status},
				{"title",               r.title},
				{"file_path",           r.file_path},
				{"duration_ms",         r.duration_ms},
			};
			if (!r.show_title.empty()) {
				j["show_title"]  = r.show_title;
				j["show_id"]     = r.show_id;
				j["season"]      = r.season;
				j["episode_num"] = r.episode;
			}
			arr.push_back(j);
		}
		route::ok(res, arr.dump());
	  } catch (const std::exception& e) {
		route::logErr("GET /api/channels/epg", e); route::err(res, 500, e.what());
	  }
	});
}
