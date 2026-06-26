#include "ConfigService.h"
#include "../RouteHelpers.h"
#include "../../conf/ConfStore.h"
#include "../../db/Database.h"
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

	svr.Get("/api/config/settings", [this](const Req&, Res& res) {
		route::ok(res, json{
			{"epg_debug",    g_epg_debug.load()},
			{"sync_threads", sync_.getThreadCount()},
		}.dump());
	});

	svr.Patch("/api/config/settings", [this](const Req& req, Res& res) {
		try {
			auto b = json::parse(req.body);
			if (b.contains("epg_debug") && b["epg_debug"].is_boolean())
				g_epg_debug.store(b["epg_debug"].get<bool>());
			if (b.contains("sync_threads") && b["sync_threads"].is_number_integer()) {
				int n = b["sync_threads"].get<int>();
				if (n >= 1 && n <= 32) sync_.setThreadCount(n);
			}
			route::ok(res, json{
				{"epg_debug",    g_epg_debug.load()},
				{"sync_threads", sync_.getThreadCount()},
			}.dump());
		} catch (const std::exception& e) {
			route::err(res, 400, e.what());
		}
	});

	svr.Post("/api/config/epg/clear-all", [this](const Req&, Res& res) {
		try {
			SQLite::Statement q(db_.get(), "DELETE FROM scheduled_program");
			int affected = q.exec();
			route::ok(res, json{{"cleared", affected}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/config/epg/clear-all", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Get("/api/config/credentials", [this](const Req&, Res& res) {
		SQLite::Statement q(db_.get(),
			"SELECT source_id, source_type, display_name FROM media_source ORDER BY display_name");
		json result = json::array();
		while (q.executeStep()) {
			auto sid = q.getColumn(0).getString();
			result.push_back({
				{"source_id",    sid},
				{"source_type",  q.getColumn(1).getString()},
				{"display_name", q.getColumn(2).getString()},
				{"has_token",    conf_.hasToken(sid)},
				{"has_user_id",  conf_.hasUserId(sid)},
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
