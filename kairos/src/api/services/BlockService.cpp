#include "BlockService.h"
#include "../RouteHelpers.h"
#include "../ScheduleCache.h"
#include "../ServiceContext.h"
#include "../../conf/ConfStore.h"
#include "../../db/BlockRepository.h"
#include "../../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "../../db/PlaylistRepository.h"
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

BlockService::BlockService(const ServiceContext& ctx)
	: db_(ctx.db), schedule_cache_(ctx.schedule_cache), conf_(ctx.conf)
{}

void BlockService::registerRoutes(httplib::Server& svr) {

	// ── Blocks ────────────────────────────────────────────────────────────────

	svr.Get("/api/channels/:id/blocks", [this](const Req& req, Res& res) {
		try {
			BlockRepository repo(db_);
			route::ok(res, repo.listWithContent(req.path_params.at("id")).dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/channels/:id/blocks", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/channels/:id/blocks", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			BlockRepository repo(db_);
			std::string block_id = repo.createBlock(channel_id, b);
			schedule_cache_.clear(channel_id);
			res.status = 201;
			route::ok(res, json{{"block_id", block_id}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/channels/:id/blocks", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Patch("/api/channels/:id/blocks/:bid", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto bid        = req.path_params.at("bid");
		try {
			auto b = json::parse(req.body);
			BlockRepository repo(db_);
			if (b.contains("name"))                     repo.updateBlockField(bid, "name",                    b["name"].get<std::string>());
			if (b.contains("block_type"))               repo.updateBlockField(bid, "block_type",              b["block_type"].get<std::string>());
			if (b.contains("day_mask"))                 repo.updateBlockField(bid, "day_mask",                (int)b["day_mask"]);
			if (b.contains("start_time"))               repo.updateBlockField(bid, "start_time",              b["start_time"].get<std::string>());
			if (b.contains("priority"))                 repo.updateBlockField(bid, "priority",                (int)b["priority"]);
			if (b.contains("program_count"))            repo.updateBlockField(bid, "program_count",           (int)b["program_count"]);
			if (b.contains("late_start_mins"))          repo.updateBlockField(bid, "late_start_mins",         (int)b["late_start_mins"]);
			if (b.contains("advancement"))              repo.updateBlockField(bid, "advancement",             b["advancement"].get<std::string>());
			if (b.contains("cursor_scope"))             repo.updateBlockField(bid, "cursor_scope",            b["cursor_scope"].get<std::string>());
			if (b.contains("max_content_rating"))       repo.updateBlockField(bid, "max_content_rating",      b["max_content_rating"].get<std::string>());
			if (b.contains("align_to_mins"))            repo.updateBlockField(bid, "align_to_mins",           (int)b["align_to_mins"]);
			if (b.contains("early_start_secs"))         repo.updateBlockField(bid, "early_start_secs",        (int)b["early_start_secs"]);
			if (b.contains("filler_selection"))         repo.updateBlockField(bid, "filler_selection",        b["filler_selection"].get<std::string>());
			if (b.contains("smart_pct"))                repo.updateBlockField(bid, "smart_pct",               (int)b["smart_pct"]);
			if (b.contains("start_scope"))              repo.updateBlockField(bid, "start_scope",             b["start_scope"].get<std::string>());
			if (b.contains("no_history_behavior"))      repo.updateBlockField(bid, "no_history_behavior",     b["no_history_behavior"].get<std::string>());
			if (b.contains("intro_content_type"))       repo.updateBlockField(bid, "intro_content_type",      b["intro_content_type"].get<std::string>());
			if (b.contains("intro_content_id"))         repo.updateBlockField(bid, "intro_content_id",        b["intro_content_id"].get<std::string>());
			if (b.contains("outro_content_type"))       repo.updateBlockField(bid, "outro_content_type",      b["outro_content_type"].get<std::string>());
			if (b.contains("outro_content_id"))         repo.updateBlockField(bid, "outro_content_id",        b["outro_content_id"].get<std::string>());
			if (b.contains("interstitial_content_type")) repo.updateBlockField(bid, "interstitial_content_type", b["interstitial_content_type"].get<std::string>());
			if (b.contains("interstitial_content_id"))  repo.updateBlockField(bid, "interstitial_content_id", b["interstitial_content_id"].get<std::string>());
			if (b.contains("interstitial_every_n"))     repo.updateBlockField(bid, "interstitial_every_n",    (int)b["interstitial_every_n"]);
			if (b.contains("max_consecutive_episodes")) repo.updateBlockField(bid, "max_consecutive_episodes",(int)b["max_consecutive_episodes"]);
			if (b.contains("inter_filler"))
				repo.updateBlockField(bid, "inter_filler",
					b["inter_filler"].is_boolean() ? (b["inter_filler"].get<bool>() ? 1 : 0)
					                               : b["inter_filler"].get<int>());
			if (b.contains("snap_to_group_start"))
				repo.updateBlockField(bid, "snap_to_group_start",
					b["snap_to_group_start"].is_boolean() ? (b["snap_to_group_start"].get<bool>() ? 1 : 0)
					                                      : b["snap_to_group_start"].get<int>());
			if (b.contains("end_time")) {
				if (b["end_time"].is_null() || b["end_time"].get<std::string>().empty())
					repo.clearBlockField(bid, "end_time");
				else
					repo.updateBlockField(bid, "end_time", b["end_time"].get<std::string>());
			}
			schedule_cache_.clear(channel_id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/channels/:id/blocks/" + bid, e);
			route::err(res, 400, e.what());
		}
	});

	svr.Delete("/api/channels/:id/blocks/:bid", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto bid        = req.path_params.at("bid");
		try {
			BlockRepository repo(db_);
			repo.removeBlock(bid);
			schedule_cache_.clear(channel_id);
			route::ok(res, json{{"deleted", bid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/channels/:id/blocks/" + bid, e);
			route::err(res, 500, e.what());
		}
	});

	// ── Block content ─────────────────────────────────────────────────────────

	svr.Post("/api/channels/:id/blocks/:bid/content", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto bid        = req.path_params.at("bid");
		try {
			auto b = json::parse(req.body);
			BlockRepository repo(db_);
			auto [rowid, position] = repo.addContent(bid, b);
			schedule_cache_.clear(channel_id);
			res.status = 201;
			route::ok(res, json{{"id", rowid}, {"position", position}}.dump());
		} catch (const SQLite::Exception& e) {
			route::logErr("POST /api/channels/:id/blocks/:bid/content", e);
			route::err(res, 409, e.what());
		} catch (const std::exception& e) {
			route::logErr("POST /api/channels/:id/blocks/:bid/content", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Patch("/api/channels/:id/blocks/:bid/content/:cid", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto cid_str    = req.path_params.at("cid");
		try {
			auto b   = json::parse(req.body);
			int  cid = std::stoi(cid_str);
			BlockRepository repo(db_);
			if (b.contains("season_filter")) {
				if (b["season_filter"].is_null())
					repo.clearContentField(cid, "season_filter");
				else
					repo.updateContentField(cid, "season_filter", b["season_filter"].get<int>());
			}
			if (b.contains("position"))        repo.updateContentField(cid, "position",        b["position"].get<int>());
			if (b.contains("weight"))          repo.updateContentField(cid, "weight",          b["weight"].get<int>());
			if (b.contains("run_count"))       repo.updateContentField(cid, "run_count",       b["run_count"].get<int>());
			if (b.contains("include_specials"))repo.updateContentField(cid, "include_specials",b["include_specials"].get<bool>() ? 1 : 0);
			if (b.contains("episode_order"))   repo.updateContentField(cid, "episode_order",   b["episode_order"].get<std::string>());
			schedule_cache_.clear(channel_id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/channels/:id/blocks/:bid/content/" + cid_str, e);
			route::err(res, 400, e.what());
		}
	});

	svr.Delete("/api/channels/:id/blocks/:bid/content/:cid", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto cid_str    = req.path_params.at("cid");
		try {
			int cid = std::stoi(cid_str);
			BlockRepository repo(db_);
			repo.removeContent(cid);
			schedule_cache_.clear(channel_id);
			route::ok(res, json{{"deleted", cid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/channels/:id/blocks/:bid/content/" + cid_str, e);
			route::err(res, 500, e.what());
		}
	});

	svr.Delete("/api/channels/:id/blocks/:bid/content/:cid/cursor", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto block_id   = req.path_params.at("bid");
		auto cid_str    = req.path_params.at("cid");
		try {
			int cid = std::stoi(cid_str);
			BlockRepository repo(db_);
			repo.resetContentCursor(channel_id, block_id, cid);
			schedule_cache_.clear(channel_id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::invalid_argument& e) {
			route::err(res, 400, e.what());
		} catch (const std::runtime_error& e) {
			route::err(res, 404, e.what());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/channels/:id/blocks/:bid/content/:cid/cursor", e);
			route::err(res, 500, e.what());
		}
	});

	// Create Kairos playlist from block content, optionally mirrored to Plex.
	svr.Post("/api/channels/:id/blocks/:bid/playlist", [this](const Req& req, Res& res) {
		auto bid = req.path_params.at("bid");
		try {
			auto b = json::parse(req.body);
			std::string title     = b.value("title",     "");
			std::string source_id = b.value("source_id", "");
			if (title.empty()) { route::err(res, 400, "title required"); return; }

			// Expand block content to {item_type, item_id} pairs.
			std::vector<std::pair<std::string, std::string>> items;
			{
				SQLite::Statement cq(db_.get(), R"(
					SELECT content_type, content_id, season_filter, episode_order, include_specials
					FROM block_content WHERE block_id = ? ORDER BY position
				)");
				cq.bind(1, bid);
				while (cq.executeStep()) {
					std::string ct  = cq.getColumn(0).getString();
					std::string cid = cq.getColumn(1).getString();
					if (ct == "show") {
						bool        has_season    = !cq.getColumn(2).isNull();
						int         season_filter = has_season ? cq.getColumn(2).getInt() : -1;
						bool        incl_specials = cq.getColumn(4).getInt() != 0;
						std::string ep_order      = cq.getColumn(3).getString();
						std::string order_col     = (ep_order == "air_date") ? "air_date" : "season, episode";
						std::string where_extra   = has_season     ? " AND season = ?"
						                          : !incl_specials ? " AND season > 0"
						                          : "";
						SQLite::Statement eq(db_.get(),
							"SELECT episode_id FROM episode WHERE show_id = ?" +
							where_extra + " ORDER BY " + order_col);
						eq.bind(1, cid);
						if (has_season) eq.bind(2, season_filter);
						while (eq.executeStep())
							items.push_back({"episode", eq.getColumn(0).getString()});
					} else if (ct == "movie") {
						items.push_back({"movie", cid});
					}
				}
			}

			PlaylistRepository repo(db_);
			std::string playlist_id = repo.create(title);
			int count = repo.addItems(playlist_id, items);
			json result = {{"playlist_id", playlist_id}, {"item_count", count}};

			if (!source_id.empty() && !items.empty()) {
				// Warn if items span multiple sources; can't merge into one Plex playlist.
				std::unordered_set<std::string> item_sources;
				for (const auto& [itype, iid] : items) {
					SQLite::Statement sq(db_.get(),
						"SELECT source_id FROM source_mapping WHERE kairos_id = ? LIMIT 1");
					sq.bind(1, iid);
					if (sq.executeStep()) item_sources.insert(sq.getColumn(0).getString());
					if (item_sources.size() > 1) break;
				}
				if (item_sources.size() > 1) {
					result["multi_source_warning"] = "items span multiple sources; Plex sync skipped";
					res.status = 201; route::ok(res, result.dump()); return;
				}

				std::string base_url, src_type;
				{
					SQLite::Statement sq(db_.get(),
						"SELECT base_url, source_type FROM media_source WHERE source_id = ?");
					sq.bind(1, source_id);
					if (!sq.executeStep() || (src_type = sq.getColumn(1).getString()) != "plex") {
						res.status = 201; route::ok(res, result.dump()); return;
					}
					base_url = sq.getColumn(0).getString();
				}

				std::string token = conf_.token(source_id);
				httplib::Client plex(base_url);
				plex.set_default_headers({{"X-Plex-Token", token}, {"Accept", "application/json"}});
				plex.set_connection_timeout(10);
				plex.set_read_timeout(30);

				std::string machine_id;
				if (auto r = plex.Get("/"); r && r->status == 200) {
					try { machine_id = json::parse(r->body)["MediaContainer"].value("machineIdentifier", ""); }
					catch (...) {}
				}

				if (!machine_id.empty()) {
					std::vector<std::string> rating_keys;
					for (const auto& [itype, iid] : items) {
						SQLite::Statement lk(db_.get(),
							"SELECT external_id FROM source_mapping "
							"WHERE source_id = ? AND kairos_id = ? AND item_type = ?");
						lk.bind(1, source_id); lk.bind(2, iid); lk.bind(3, itype);
						if (lk.executeStep()) rating_keys.push_back(lk.getColumn(0).getString());
					}

					if (!rating_keys.empty()) {
						std::string keys_csv;
						for (size_t i = 0; i < rating_keys.size(); ++i) {
							if (i) keys_csv += ',';
							keys_csv += rating_keys[i];
						}

						auto urlEncode = [](const std::string& s) {
							std::string out;
							for (unsigned char c : s) {
								if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
									out += c;
								else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", c); out += buf; }
							}
							return out;
						};

						std::string uri  = "server://" + machine_id +
							"/com.plexapp.plugins.library/library/metadata/" + keys_csv;
						std::string path = "/playlists?type=video&title=" + urlEncode(title) +
							"&smart=0&uri=" + urlEncode(uri) + "&machineIdentifier=" + machine_id;

						if (auto r = plex.Post(path.c_str(), "", "application/x-www-form-urlencoded");
							r && (r->status == 200 || r->status == 201)) {
							try {
								const auto& meta = json::parse(r->body)["MediaContainer"]["Metadata"];
								if (meta.is_array() && !meta.empty()) {
									std::string plex_id = meta[0]["ratingKey"].get<std::string>();
									int64_t now = static_cast<int64_t>(std::time(nullptr));
									SQLite::Statement ul(db_.get(), R"(
										INSERT INTO plex_list_link
											(list_type, list_id, source_id, external_id, plex_type, last_synced_at)
										VALUES ('playlist',?,?,?,'playlist',?)
										ON CONFLICT(list_type, list_id) DO UPDATE SET
											source_id = excluded.source_id, external_id = excluded.external_id,
											plex_type = excluded.plex_type, last_synced_at = excluded.last_synced_at
									)");
									ul.bind(1, playlist_id); ul.bind(2, source_id);
									ul.bind(3, plex_id); ul.bind(4, now);
									ul.exec();
									result["plex_playlist_id"] = plex_id;
								}
							} catch (...) {}
						}
					}
				}
			}

			res.status = 201;
			route::ok(res, result.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/channels/:id/blocks/:bid/playlist", e);
			route::err(res, 400, e.what());
		}
	});

	// ── Block filler entries ──────────────────────────────────────────────────

	svr.Post("/api/channels/:id/blocks/:bid/filler", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto block_id   = req.path_params.at("bid");
		try {
			auto b = json::parse(req.body);
			if (b.value("content_id", "").empty()) { route::err(res, 400, "content_id required"); return; }
			BlockRepository repo(db_);
			auto r = repo.addFillerEntry(block_id, b);
			schedule_cache_.clear(channel_id);

			std::string ct  = b.value("content_type", "filler_list");
			std::string cid = b.value("content_id",   "");
			json resp = {
				{"id",           r.id},
				{"content_type", ct},
				{"content_id",   cid},
				{"title",        r.title},
				{"advancement",  b.value("advancement", "sequential")},
				{"weight",       b.value("weight",      1)},
				{"position",     r.position},
			};
			if (b.contains("season_filter") && !b["season_filter"].is_null())
				resp["season_filter"] = b["season_filter"].get<int>();
			res.status = 201;
			route::ok(res, resp.dump());
		} catch (const SQLite::Exception& e) {
			route::logErr("POST /api/channels/:id/blocks/:bid/filler", e);
			route::err(res, 409, e.what());
		} catch (const std::exception& e) {
			route::logErr("POST /api/channels/:id/blocks/:bid/filler", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Patch("/api/channels/:id/blocks/:bid/filler/:eid", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto eid_str    = req.path_params.at("eid");
		try {
			auto b   = json::parse(req.body);
			int  eid = std::stoi(eid_str);
			BlockRepository repo(db_);
			if (b.contains("advancement")) repo.updateFillerEntryField(eid, "advancement", b["advancement"].get<std::string>());
			if (b.contains("weight"))      repo.updateFillerEntryField(eid, "weight",      b["weight"].get<int>());
			schedule_cache_.clear(channel_id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/channels/:id/blocks/:bid/filler/" + eid_str, e);
			route::err(res, 400, e.what());
		}
	});

	svr.Delete("/api/channels/:id/blocks/:bid/filler/:eid", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto eid_str    = req.path_params.at("eid");
		try {
			int eid = std::stoi(eid_str);
			BlockRepository repo(db_);
			repo.removeFillerEntry(eid);
			schedule_cache_.clear(channel_id);
			route::ok(res, json{{"deleted", eid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/channels/:id/blocks/:bid/filler/" + eid_str, e);
			route::err(res, 500, e.what());
		}
	});

	// ── Channel bumpers ───────────────────────────────────────────────────────

	svr.Get("/api/channels/:id/bumpers", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		try {
			SQLite::Statement q(db_.get(), R"(
				SELECT id, content_type, content_id, mode, every_n, position,
				       CASE content_type
				         WHEN 'show'     THEN (SELECT title FROM show     WHERE show_id     = content_id)
				         WHEN 'playlist' THEN (SELECT title FROM playlist WHERE playlist_id = content_id)
				         WHEN 'episode'  THEN (SELECT title FROM episode  WHERE episode_id  = content_id)
				         ELSE content_id
				       END AS title,
				       season_filter
				FROM channel_bumper WHERE channel_id = ? ORDER BY position
			)");
			q.bind(1, channel_id);
			json result = json::array();
			while (q.executeStep()) {
				json bumper = {
					{"id",           q.getColumn(0).getInt()},
					{"channel_id",   channel_id},
					{"content_type", q.getColumn(1).getString()},
					{"content_id",   q.getColumn(2).getString()},
					{"mode",         q.getColumn(3).getString()},
					{"every_n",      q.getColumn(4).getInt()},
					{"position",     q.getColumn(5).getInt()},
					{"title",        q.getColumn(6).isNull() ? q.getColumn(2).getString() : q.getColumn(6).getString()},
				};
				if (!q.getColumn(7).isNull()) bumper["season_filter"] = q.getColumn(7).getInt();
				result.push_back(bumper);
			}
			route::ok(res, result.dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/channels/:id/bumpers", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/channels/:id/bumpers", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::string ct      = b.value("content_type", "show");
			std::string cid     = b.value("content_id",   "");
			std::string mode    = b.value("mode",         "between");
			int         every_n = b.value("every_n",      3);
			std::optional<int> sf;
			if (b.contains("season_filter") && !b["season_filter"].is_null())
				sf = b["season_filter"].get<int>();
			BlockRepository repo(db_);
			auto r = repo.addBumper(channel_id, ct, cid, mode, every_n, sf);
			schedule_cache_.clear(channel_id);
			json resp = {
				{"id",           r.id},
				{"channel_id",   channel_id},
				{"content_type", ct},
				{"content_id",   cid},
				{"mode",         mode},
				{"every_n",      every_n},
				{"position",     r.position},
				{"title",        r.title},
			};
			if (sf.has_value()) resp["season_filter"] = sf.value();
			res.status = 201;
			route::ok(res, resp.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/channels/:id/bumpers", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Patch("/api/channels/:id/bumpers/:bid", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto bid_str    = req.path_params.at("bid");
		try {
			auto b   = json::parse(req.body);
			int  bid = std::stoi(bid_str);
			BlockRepository repo(db_);
			if (b.contains("content_type")) repo.updateBumperField(bid, "content_type", b["content_type"].get<std::string>());
			if (b.contains("content_id"))   repo.updateBumperField(bid, "content_id",   b["content_id"].get<std::string>());
			if (b.contains("mode"))         repo.updateBumperField(bid, "mode",         b["mode"].get<std::string>());
			if (b.contains("every_n"))      repo.updateBumperField(bid, "every_n",      b["every_n"].get<int>());
			if (b.contains("position"))     repo.updateBumperField(bid, "position",     b["position"].get<int>());
			schedule_cache_.clear(channel_id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/channels/:id/bumpers/" + bid_str, e);
			route::err(res, 400, e.what());
		}
	});

	svr.Delete("/api/channels/:id/bumpers/:bid", [this](const Req& req, Res& res) {
		auto channel_id = req.path_params.at("id");
		auto bid_str    = req.path_params.at("bid");
		try {
			int bid = std::stoi(bid_str);
			BlockRepository repo(db_);
			repo.removeBumper(bid);
			schedule_cache_.clear(channel_id);
			route::ok(res, json{{"deleted", bid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/channels/:id/bumpers/" + bid_str, e);
			route::err(res, 500, e.what());
		}
	});

}
