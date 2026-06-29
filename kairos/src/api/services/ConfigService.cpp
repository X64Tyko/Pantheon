#include "ConfigService.h"
#include "../RouteHelpers.h"
#include "../../conf/ConfStore.h"
#include "../../db/ScheduleRepository.h"
#include "../../db/SourceRepository.h"
#include "../../source/SyncManager.h"
#include "../../scheduler/RuntimeFlags.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

ConfigService::ConfigService(const ServiceContext& ctx)
	: db_(ctx.db), conf_(ctx.conf), sync_(ctx.sync) {}

void ConfigService::registerRoutes(httplib::Server& svr) {

	auto persistFlag = [this](const char* key, bool value) {
		SQLite::Statement s(db_.get(),
			"INSERT INTO app_config(key,value) VALUES(?,?)"
			" ON CONFLICT(key) DO UPDATE SET value=excluded.value");
		s.bind(1, std::string(key));
		s.bind(2, value ? "1" : "0");
		s.exec();
	};

	auto settingsJson = [this]() {
		return json{
			{"epg_debug",              g_epg_debug.load()},
			{"sync_debug",             g_debug_logging.load()},
			{"sync_threads",           sync_.getThreadCount()},
			{"image_cache_ttl_hours",  conf_.getImageCacheTtlHours()},
		};
	};

	svr.Get("/api/config/settings", [settingsJson](const Req&, Res& res) {
		route::ok(res, settingsJson().dump());
	});

	svr.Patch("/api/config/settings", [this, persistFlag, settingsJson](const Req& req, Res& res) {
		try {
			auto b = json::parse(req.body);
			if (b.contains("epg_debug") && b["epg_debug"].is_boolean()) {
				bool v = b["epg_debug"].get<bool>();
				g_epg_debug.store(v);
				persistFlag("epg_debug", v);
			}
			if (b.contains("sync_debug") && b["sync_debug"].is_boolean()) {
				bool v = b["sync_debug"].get<bool>();
				g_debug_logging.store(v);
				persistFlag("sync_debug", v);
				std::cout << "[config] sync debug logging " << (v ? "enabled" : "disabled") << '\n';
			}
			if (b.contains("sync_threads") && b["sync_threads"].is_number_integer()) {
				int n = b["sync_threads"].get<int>();
				if (n >= 1 && n <= 32) sync_.setThreadCount(n);
			}
			if (b.contains("image_cache_ttl_hours") && b["image_cache_ttl_hours"].is_number_integer()) {
				int h = b["image_cache_ttl_hours"].get<int>();
				if (h >= 1 && h <= 720) conf_.setImageCacheTtlHours(h);
			}
			route::ok(res, settingsJson().dump());
		} catch (const std::exception& e) {
			route::err(res, 400, e.what());
		}
	});

	svr.Post("/api/config/epg/clear-all", [this](const Req&, Res& res) {
		try {
			int affected = ScheduleRepository(db_).clearAllScheduled();
			route::ok(res, json{{"cleared", affected}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/config/epg/clear-all", e);
			route::err(res, 500, e.what());
		}
	});

	// Reset the entire media library index — wipes all show/episode/movie data and
	// source mappings so the next sync starts completely fresh. Keeps source/library
	// configuration, channels, users, and settings intact.
	svr.Post("/api/config/library/reset", [this](const Req&, Res& res) {
		try {
			SQLite::Transaction txn(db_.get());

			// Null out FK columns in media_cursor before deleting the rows they point at.
			db_.get().exec("UPDATE media_cursor SET episode_id = NULL WHERE episode_id IS NOT NULL");
			db_.get().exec("UPDATE media_cursor SET movie_id   = NULL WHERE movie_id   IS NOT NULL");

			// Scraper match/override data tied to kairos IDs.
			db_.get().exec("DELETE FROM item_match_candidate");
			db_.get().exec("DELETE FROM metadata_override");
			db_.get().exec("DELETE FROM scraper_job");

			// Chapters are keyed by episode file path; stale after ID reassignment.
			db_.get().exec("DELETE FROM chapter");

			// Playlists are source-synced; they'll be recreated on next sync.
			db_.get().exec("DELETE FROM playlist_item");
			db_.get().exec("DELETE FROM playlist");

			// Core media tables — delete in FK order (episode → show).
			db_.get().exec("DELETE FROM episode");
			db_.get().exec("DELETE FROM show_season");
			db_.get().exec("DELETE FROM show");
			db_.get().exec("DELETE FROM movie");
			db_.get().exec("DELETE FROM source_mapping");

			txn.commit();

			// Reload sources so SyncManager's in-memory list is consistent.
			sync_.loadSources();

			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/config/library/reset", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Get("/api/config/credentials", [this](const Req&, Res& res) {
		json result = json::array();
		for (const auto& r : SourceRepository(db_).listSourcesBasic()) {
			result.push_back({
				{"source_id",    r.source_id},
				{"source_type",  r.source_type},
				{"display_name", r.display_name},
				{"has_token",    conf_.hasToken(r.source_id)},
				{"has_user_id",  conf_.hasUserId(r.source_id)},
			});
		}
		route::ok(res, result.dump());
	});

	svr.Get("/api/config/credentials/:source_id", [this](const Req& req, Res& res) {
		auto sid = req.path_params.at("source_id");
		route::ok(res, json{{"has_token",   conf_.hasToken(sid)},
		                    {"has_user_id", conf_.hasUserId(sid)}}.dump());
	});

	svr.Put("/api/config/credentials/:source_id", [this](const Req& req, Res& res) {
		try {
			auto sid     = req.path_params.at("source_id");
			auto b       = json::parse(req.body);
			auto token   = b.value("token",   "");
			auto user_id = b.value("user_id", "");
			conf_.setCredentials(sid, token, user_id);
			sync_.loadSources();
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("PUT /api/config/credentials/:source_id", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Delete("/api/config/credentials/:source_id", [this](const Req& req, Res& res) {
		auto sid = req.path_params.at("source_id");
		try {
			conf_.removeSource(sid);
			sync_.loadSources();
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/config/credentials/" + sid, e);
			route::err(res, 500, e.what());
		}
	});

	svr.Get("/api/config/path-maps/:source_id", [this](const Req& req, Res& res) {
		auto sid  = req.path_params.at("source_id");
		auto maps = conf_.getPathMaps(sid);
		json result = json::array();
		for (const auto& [from, to] : maps)
			result.push_back({{"from", from}, {"to", to}});
		route::ok(res, result.dump());
	});

	svr.Put("/api/config/path-maps/:source_id", [this](const Req& req, Res& res) {
		try {
			auto sid = req.path_params.at("source_id");
			auto b   = json::parse(req.body);
			std::vector<std::pair<std::string,std::string>> maps;
			if (b.contains("maps") && b["maps"].is_array()) {
				for (const auto& m : b["maps"]) {
					auto from = m.value("from", "");
					auto to   = m.value("to",   "");
					if (!from.empty()) maps.push_back({from, to});
				}
			}
			conf_.setPathMaps(sid, maps);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const json::exception& e) { route::err(res, 400, e.what()); }
		  catch (const std::exception& e)  {
			route::logErr("PUT /api/config/path-maps/:source_id", e);
			route::err(res, 500, e.what());
		}
	});
}
