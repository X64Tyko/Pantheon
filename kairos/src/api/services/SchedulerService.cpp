#include "SchedulerService.h"
#include "../RouteHelpers.h"
#include "../ScheduleCache.h"
#include "../ServiceContext.h"
#include "../../conf/ConfStore.h"
#include "../../db/Database.h"
#include "../../scheduler/EPGMaterializer.h"
#include "../../db/DbHelpers.h"
#include "../../scheduler/CursorState.h"
#include "../../scheduler/RuleEngine.h"
#include <SQLiteCpp/SQLiteCpp.h>
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

void SchedulerService::attachSourceMapping(json& j, const std::string& item_id) {
	try {
		SQLite::Statement sm(db_.get(),
			"SELECT source_id, external_id FROM source_mapping WHERE kairos_id = ? LIMIT 1");
		sm.bind(1, item_id);
		if (sm.executeStep()) {
			j["source_id"]   = sm.getColumn(0).getString();
			j["external_id"] = sm.getColumn(1).getString();
		}
	} catch (...) {}
}

void SchedulerService::registerRoutes(httplib::Server& svr) {

	svr.Get("/playlist.m3u", [this](const Req& req, Res& res) {
		std::string host = req.get_header_value("Host");
		if (host.empty()) host = "localhost:8080";
		res.set_content(materializer_.generateM3U("http://" + host), "application/x-mpegURL");
	});

	svr.Get("/epg.xml", [this](const Req& req, Res& res) {
		int hours = 24;
		if (req.has_param("hours")) {
			try { hours = std::stoi(req.get_param_value("hours")); } catch (...) {}
		}
		hours = std::max(1, std::min(hours, 72));
		res.set_content(materializer_.generateXMLTV(hours), "application/xml");
	});

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

		{
			SQLite::Statement q(db_.get(), R"(
				SELECT sp.item_type, sp.item_id,
				       COALESCE(sp.block_id, '')          AS block_id,
				       sp.wall_clock_start,
				       COALESCE(e.title,  m.title,  '')   AS title,
				       COALESCE(s.title,  '')             AS show_title,
				       COALESCE(e.show_id,'')             AS show_id,
				       COALESCE(e.season, 0)              AS season,
				       COALESCE(e.episode,0)              AS ep_num,
				       COALESCE(e.file_path, m.file_path,'') AS file_path,
				       COALESCE(e.duration_ms, m.duration_ms, 0) AS duration_ms,
				       sp.wall_clock_end,
				       sp.is_filler
				FROM scheduled_program sp
				LEFT JOIN episode e ON sp.item_type='episode' AND sp.item_id=e.episode_id
				LEFT JOIN show    s ON sp.item_type='episode' AND e.show_id =s.show_id
				LEFT JOIN movie   m ON sp.item_type='movie'   AND sp.item_id=m.movie_id
				WHERE sp.channel_id    = ?
				  AND sp.wall_clock_start <= ?
				  AND sp.wall_clock_end   >  ?
				  AND sp.status != 'skipped'
				ORDER BY sp.wall_clock_start DESC
				LIMIT 1
			)");
			q.bind(1, channel_id);
			q.bind(2, static_cast<int64_t>(t));
			q.bind(3, static_cast<int64_t>(t));
			if (q.executeStep()) {
				std::string item_type  = q.getColumn(0).getString();
				std::string item_id    = q.getColumn(1).getString();
				std::string block_id   = q.getColumn(2).getString();
				int64_t     wall_start = q.getColumn(3).getInt64();
				std::string title      = q.getColumn(4).getString();
				std::string show_title = q.getColumn(5).getString();
				std::string show_id    = q.getColumn(6).getString();
				int         season     = q.getColumn(7).getInt();
				int         ep_num     = q.getColumn(8).getInt();
				std::string file_path  = q.getColumn(9).getString();
				int64_t     duration_ms= q.getColumn(10).getInt64();
				int64_t     wall_end   = q.getColumn(11).getInt64();
				bool        is_filler  = q.getColumn(12).getInt() != 0;

				json j = {
					{"item_type",           item_type},
					{"item_id",             item_id},
					{"file_path",           conf_.applyPathMap(file_path)},
					{"duration_ms",         duration_ms},
					{"title",               title},
					{"block_id",            block_id},
					{"wall_clock_start_ms", wall_start * 1000LL},
					{"wall_clock_end_ms",   wall_end   * 1000LL},
					{"is_filler",           is_filler},
				};
				if (!show_title.empty()) {
					j["show_title"]  = show_title;
					j["show_id"]     = show_id;
					j["season"]      = season;
					j["episode_num"] = ep_num;
				}
				attachSourceMapping(j, item_id);
				route::ok(res, j.dump());
				return;
			}
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
			attachSourceMapping(j, item.item_id);
			route::ok(res, j.dump());
			return;
		}

		// Fallback 1: random item from the channel's default filler lists.
		{
			SQLite::Statement fq(db_.get(), R"(
				SELECT fi.item_type, fi.item_id,
				       COALESCE(e.file_path, m.file_path, '') AS file_path,
				       COALESCE(e.title,     m.title,     '') AS title,
				       COALESCE(e.duration_ms, m.duration_ms, 0) AS duration_ms
				FROM channel_filler_entry cfe
				JOIN filler_list_item fi ON fi.filler_list_id = cfe.filler_list_id
				LEFT JOIN episode e ON fi.item_type='episode' AND fi.item_id=e.episode_id
				LEFT JOIN movie   m ON fi.item_type='movie'   AND fi.item_id=m.movie_id
				WHERE cfe.channel_id = ?
				  AND (e.file_path IS NOT NULL OR m.file_path IS NOT NULL)
				ORDER BY RANDOM()
				LIMIT 1
			)");
			fq.bind(1, channel_id);
			if (fq.executeStep()) {
				int64_t dur = fq.getColumn(4).getInt64();
				json j = {
					{"item_type",           fq.getColumn(0).getString()},
					{"item_id",             fq.getColumn(1).getString()},
					{"file_path",           conf_.applyPathMap(fq.getColumn(2).getString())},
					{"title",               fq.getColumn(3).getString()},
					{"duration_ms",         dur},
					{"block_id",            ""},
					{"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
					{"wall_clock_end_ms",   static_cast<int64_t>(t) * 1000 + dur},
					{"is_filler",           true},
				};
				route::ok(res, j.dump());
				return;
			}
		}

		// Fallback 2: channel-configured offline screen.
		{
			SQLite::Statement oq(db_.get(),
				"SELECT offline_video_path, offline_image_path, "
				"       offline_audio_id, offline_audio_type "
				"FROM channel WHERE channel_id = ?");
			oq.bind(1, channel_id);
			if (oq.executeStep()) {
				std::string vid_path  = oq.getColumn(0).getString();
				std::string img_path  = oq.getColumn(1).getString();
				std::string audio_id  = oq.getColumn(2).getString();
				std::string audio_typ = oq.getColumn(3).getString();

				if (!vid_path.empty()) {
					route::ok(res, json{
						{"item_type",           "offline"},
						{"file_path",           conf_.applyPathMap(vid_path)},
						{"duration_ms",         0},
						{"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
					}.dump());
					return;
				}
				if (!img_path.empty()) {
					json j = {
						{"item_type",           "offline"},
						{"offline_image_path",  conf_.applyPathMap(img_path)},
						{"duration_ms",         0},
						{"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
					};
					if (!audio_id.empty() && !audio_typ.empty()) {
						const char* sql = (audio_typ == "episode")
							? "SELECT file_path FROM episode WHERE episode_id = ?"
							: "SELECT file_path FROM movie   WHERE movie_id   = ?";
						SQLite::Statement aq(db_.get(), sql);
						aq.bind(1, audio_id);
						if (aq.executeStep()) {
							std::string ap = aq.getColumn(0).getString();
							if (!ap.empty()) j["offline_audio_path"] = conf_.applyPathMap(ap);
						}
					}
					route::ok(res, j.dump());
					return;
				}
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

		int64_t current_end = static_cast<int64_t>(t);
		{
			SQLite::Statement q(db_.get(), R"(
				SELECT wall_clock_end FROM scheduled_program
				WHERE channel_id = ? AND wall_clock_start <= ? AND wall_clock_end > ?
				  AND status != 'skipped'
				ORDER BY wall_clock_start DESC LIMIT 1
			)");
			q.bind(1, channel_id);
			q.bind(2, static_cast<int64_t>(t));
			q.bind(3, static_cast<int64_t>(t));
			if (q.executeStep()) current_end = q.getColumn(0).getInt64();
		}

		SQLite::Statement q(db_.get(), R"(
			SELECT sp.item_type, sp.item_id, COALESCE(sp.block_id, ''),
			       sp.wall_clock_start,
			       COALESCE(e.title, m.title, '')    AS title,
			       COALESCE(s.title, '')              AS show_title,
			       COALESCE(e.show_id, '')            AS show_id,
			       COALESCE(e.season,  0)             AS season,
			       COALESCE(e.episode, 0)             AS ep_num,
			       COALESCE(e.file_path, m.file_path, '') AS file_path,
			       COALESCE(e.duration_ms, m.duration_ms,
			                (sp.wall_clock_end - sp.wall_clock_start) * 1000) AS duration_ms
			FROM scheduled_program sp
			LEFT JOIN episode e ON sp.item_type = 'episode' AND sp.item_id = e.episode_id
			LEFT JOIN show    s ON sp.item_type = 'episode' AND e.show_id  = s.show_id
			LEFT JOIN movie   m ON sp.item_type = 'movie'   AND sp.item_id = m.movie_id
			WHERE sp.channel_id = ?
			  AND sp.wall_clock_start >= ?
			  AND sp.is_filler = 0
			  AND sp.status != 'skipped'
			ORDER BY sp.wall_clock_start
			LIMIT 1
		)");
		q.bind(1, channel_id);
		q.bind(2, current_end);

		if (!q.executeStep()) { route::err(res, 404, "no next item available"); return; }

		std::string item_type   = q.getColumn(0).getString();
		std::string item_id     = q.getColumn(1).getString();
		std::string block_id    = q.getColumn(2).getString();
		int64_t     wall_start  = q.getColumn(3).getInt64();
		std::string title       = q.getColumn(4).getString();
		std::string show_title  = q.getColumn(5).getString();
		std::string show_id     = q.getColumn(6).getString();
		int         season      = q.getColumn(7).getInt();
		int         ep_num      = q.getColumn(8).getInt();
		std::string file_path   = q.getColumn(9).getString();
		int64_t     duration_ms = q.getColumn(10).getInt64();

		json j = {
			{"item_type",           item_type},
			{"item_id",             item_id},
			{"file_path",           conf_.applyPathMap(file_path)},
			{"duration_ms",         duration_ms},
			{"title",               title},
			{"block_id",            block_id},
			{"wall_clock_start_ms", wall_start * 1000LL},
		};
		if (!show_title.empty()) {
			j["show_title"]  = show_title;
			j["show_id"]     = show_id;
			j["season"]      = season;
			j["episode_num"] = ep_num;
		}
		attachSourceMapping(j, item_id);
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
			{
				SQLite::Statement am(db_.get(),
					"SELECT advance_mode FROM channel WHERE channel_id=?");
				am.bind(1, channel_id);
				if (am.executeStep() && am.getColumn(0).getString() == "on_play")
					schedule_cache_.clear(channel_id);
			}
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
			// Temporarily inject preview blocks into DB, generate (pure), then rollback.
			db_.get().exec("SAVEPOINT preview_sp");
			try {
				SQLite::Statement delbc(db_.get(),
					"DELETE FROM block_content WHERE block_id IN (SELECT block_id FROM block WHERE channel_id=?)");
				delbc.bind(1, channel_id); delbc.exec();
				SQLite::Statement delbfe(db_.get(),
					"DELETE FROM block_filler_entry WHERE block_id IN (SELECT block_id FROM block WHERE channel_id=?)");
				delbfe.bind(1, channel_id); delbfe.exec();
				SQLite::Statement delb(db_.get(), "DELETE FROM block WHERE channel_id=?");
				delb.bind(1, channel_id); delb.exec();

				for (auto& blk : body["blocks"]) {
					std::string block_id = blk.value("block_id", db::generateId());
					std::string end_time = blk.value("end_time", "");
					SQLite::Statement s(db_.get(), R"(
						INSERT INTO block (block_id, channel_id, name, block_type, day_mask,
						                   start_time, end_time, program_count, priority,
						                   play_style, advancement, cursor_scope,
						                   late_start_mins, align_to_mins, inter_filler,
						                   early_start_secs, filler_selection, smart_pct,
						                   start_scope, no_history_behavior,
						                   max_consecutive_episodes, interstitial_every_n)
						VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
					)");
					s.bind(1, block_id); s.bind(2, channel_id);
					s.bind(3, blk.value("name", ""));
					s.bind(4, blk.value("block_type", "episode"));
					s.bind(5, blk.value("day_mask", 127));
					s.bind(6, blk.value("start_time", "00:00"));
					if (end_time.empty()) s.bind(7); else s.bind(7, end_time);
					s.bind(8,  blk.value("program_count",      0));
					s.bind(9,  blk.value("priority",           0));
					s.bind(10, blk.value("play_style",          "standard"));
					s.bind(11, blk.value("advancement",        "sequential"));
					s.bind(12, blk.value("cursor_scope",       "block"));
					s.bind(13, blk.value("late_start_mins",    0));
					s.bind(14, blk.value("align_to_mins",      0));
					s.bind(15, blk.value("inter_filler", false) ? 1 : 0);
					s.bind(16, blk.value("early_start_secs",         0));
					s.bind(17, blk.value("filler_selection",   "round_robin"));
					s.bind(18, blk.value("smart_pct",          30));
					s.bind(19, blk.value("start_scope",        "block"));
					s.bind(20, blk.value("no_history_behavior", "normal"));
					s.bind(21, blk.value("max_consecutive_episodes", 0));
					s.bind(22, blk.value("interstitial_every_n",     1));
					s.exec();

					int pos = 0;
					for (auto& item : blk.value("content", json::array())) {
						std::string content_id = item.value("content_id", "");
						std::string ct         = item.value("content_type", "");
						if (content_id.empty() || ct.empty()) { pos++; continue; }
						SQLite::Statement ins(db_.get(), R"(
							INSERT OR IGNORE INTO block_content
							    (block_id, content_type, content_id, position,
							     weight, run_count, include_specials, episode_order)
							VALUES (?,?,?,?,?,?,?,?)
						)");
						ins.bind(1, block_id); ins.bind(2, ct); ins.bind(3, content_id);
						ins.bind(4, pos);
						ins.bind(5, item.value("weight",           1));
						ins.bind(6, item.value("run_count",        1));
						ins.bind(7, item.value("include_specials", false) ? 1 : 0);
						ins.bind(8, item.value("episode_order",    "season"));
						ins.exec();
						if (item.contains("season_filter") && !item["season_filter"].is_null()) {
							SQLite::Statement upd(db_.get(), R"(
								UPDATE block_content SET season_filter = ?
								WHERE block_id = ? AND content_type = ? AND content_id = ?
							)");
							upd.bind(1, item["season_filter"].get<int>());
							upd.bind(2, block_id); upd.bind(3, ct); upd.bind(4, content_id);
							upd.exec();
						}
						pos++;
					}

					int fpos = 0;
					for (auto& fe : blk.value("filler_entries", json::array())) {
						std::string ct  = fe.value("content_type", "filler_list");
						std::string cid = fe.value("content_id",   fe.value("filler_list_id", ""));
						if (cid.empty()) { fpos++; continue; }
						SQLite::Statement ins(db_.get(), R"(
							INSERT OR IGNORE INTO block_filler_entry
							    (block_id, content_type, content_id, advancement, weight, position)
							VALUES (?,?,?,?,?,?)
						)");
						ins.bind(1, block_id); ins.bind(2, ct); ins.bind(3, cid);
						ins.bind(4, fe.value("advancement", "sized"));
						ins.bind(5, fe.value("weight", 1));
						ins.bind(6, fpos); ins.exec();
						fpos++;
					}

					// Slots (timeslot blocks only) — deleted by CASCADE when block was deleted,
					// must be re-inserted from request JSON for projection to see them.
					if (blk.value("block_type", std::string("")) == "timeslot") {
						int sidx = 0;
						for (auto& slot : blk.value("slots", json::array())) {
							std::string slot_id = slot.value("slot_id", db::generateId());
							SQLite::Statement ss(db_.get(), R"(
								INSERT OR IGNORE INTO timeslot_slot
								    (slot_id, block_id, slot_index, slot_offset_mins,
								     slot_duration_mins, overflow, late_start_mins,
								     early_start_secs, align_to_mins, start_scope)
								VALUES (?,?,?,?,?,?,?,?,?,?)
							)");
							ss.bind(1, slot_id); ss.bind(2, block_id);
							ss.bind(3, sidx++);
							ss.bind(4, slot.value("slot_offset_mins",   0));
							ss.bind(5, slot.value("slot_duration_mins", 60));
							ss.bind(6, slot.value("overflow",           std::string("cutoff")));
							ss.bind(7, slot.value("late_start_mins",    5));
							ss.bind(8, slot.value("early_start_secs",   0));
							ss.bind(9, slot.value("align_to_mins",      0));
							ss.bind(10, slot.value("start_scope",       std::string("block")));
							ss.exec();

							int qidx = 0;
							for (auto& qe : slot.value("queue", json::array())) {
								std::string entry_id = qe.value("entry_id", db::generateId());
								std::string prem     = qe.value("premiere_date", std::string(""));
								SQLite::Statement sq(db_.get(), R"(
									INSERT OR IGNORE INTO timeslot_slot_queue
									    (entry_id, slot_id, queue_index, content_type, content_id,
									     premiere_date, pre_premiere_behavior)
									VALUES (?,?,?,?,?,?,?)
								)");
								sq.bind(1, entry_id); sq.bind(2, slot_id); sq.bind(3, qidx++);
								sq.bind(4, qe.value("content_type", std::string("show")));
								sq.bind(5, qe.value("content_id",   std::string("")));
								if (prem.empty()) sq.bind(6); else sq.bind(6, prem);
								sq.bind(7, qe.value("pre_premiere_behavior",
								                    std::string("replay_previous")));
								sq.exec();
							}
						}
					}
				}

				gr = materializer_.generate(channel_id, now, hours, req_seed);
			} catch (...) {
				db_.get().exec("ROLLBACK TO SAVEPOINT preview_sp");
				db_.get().exec("RELEASE SAVEPOINT preview_sp");
				throw;
			}
			db_.get().exec("ROLLBACK TO SAVEPOINT preview_sp");
			db_.get().exec("RELEASE SAVEPOINT preview_sp");
		} else {
			gr = materializer_.generate(channel_id, now, hours, req_seed);
		}

		// Build response from gr.items — no DB read needed.
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
			// Merge persisted anchors with newly discovered ones from this generate pass.
			{
				SQLite::Statement ach(db_.get(),
					"SELECT anchor_hashes FROM channel WHERE channel_id=?");
				ach.bind(1, channel_id);
				if (ach.executeStep() && !ach.getColumn(0).isNull()) {
					try { anchors_j = json::parse(ach.getColumn(0).getString()); }
					catch (...) {}
				}
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
		SQLite::Statement q(db_.get(), R"(
			SELECT sp.item_type, sp.item_id, sp.block_id,
			       sp.wall_clock_start, sp.wall_clock_end, sp.status,
			       COALESCE(e.title, m.title, '') AS item_title,
			       COALESCE(s.title, '')           AS show_title,
			       COALESCE(e.show_id, '')         AS show_id,
			       COALESCE(e.season,  0)          AS season,
			       COALESCE(e.episode, 0)          AS ep_num,
			       COALESCE(e.file_path, m.file_path, '') AS file_path,
			       COALESCE(e.duration_ms, m.duration_ms,
			                (sp.wall_clock_end - sp.wall_clock_start) * 1000) AS duration_ms
			FROM scheduled_program sp
			LEFT JOIN episode e ON sp.item_type='episode' AND sp.item_id=e.episode_id
			LEFT JOIN show    s ON sp.item_type='episode' AND e.show_id=s.show_id
			LEFT JOIN movie   m ON sp.item_type='movie'   AND sp.item_id=m.movie_id
			WHERE sp.channel_id=?
			  AND sp.wall_clock_end   >  ?
			  AND sp.wall_clock_start <  ?
			  AND sp.status != 'skipped'
			ORDER BY sp.wall_clock_start
		)");
		q.bind(1, channel_id);
		q.bind(2, static_cast<int64_t>(now));
		q.bind(3, horizon);

		json arr = json::array();
		while (q.executeStep()) {
			json j = {
				{"item_type",           q.getColumn(0).getString()},
				{"item_id",             q.getColumn(1).getString()},
				{"block_id",            q.getColumn(2).isNull() ? "" : q.getColumn(2).getString()},
				{"wall_clock_start_ms", q.getColumn(3).getInt64() * 1000},
				{"wall_clock_end_ms",   q.getColumn(4).getInt64() * 1000},
				{"status",              q.getColumn(5).getString()},
				{"title",               q.getColumn(6).getString()},
				{"file_path",           q.getColumn(11).getString()},
				{"duration_ms",         q.getColumn(12).getInt64()},
			};
			std::string show_title = q.getColumn(7).getString();
			if (!show_title.empty()) {
				j["show_title"]  = show_title;
				j["show_id"]     = q.getColumn(8).getString();
				j["season"]      = q.getColumn(9).getInt();
				j["episode_num"] = q.getColumn(10).getInt();
			}
			arr.push_back(j);
		}
		route::ok(res, arr.dump());
	  } catch (const std::exception& e) {
		route::logErr("GET /api/channels/epg", e); route::err(res, 500, e.what());
	  }
	});
}
