#include "ArrService.h"
#include "../RouteHelpers.h"
#include "../../arr/ArrServiceFactory.h"
#include "../../arr/IArrService.h"
#include "../../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

ArrService::ArrService(const ServiceContext& ctx) : db_(ctx.db) {}

void ArrService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/config/arr", [this](const Req&, Res& res) {
		auto getVal = [&](const char* k) -> std::string {
			SQLite::Statement q(db_.get(), "SELECT value FROM app_config WHERE key = ?");
			q.bind(1, std::string(k));
			return q.executeStep() ? q.getColumn(0).getString() : "";
		};
		route::ok(res, json{
			{"sonarr_url",     getVal("sonarr_url")},
			{"sonarr_api_key", getVal("sonarr_api_key")},
			{"radarr_url",     getVal("radarr_url")},
			{"radarr_api_key", getVal("radarr_api_key")},
		}.dump());
	});

	svr.Patch("/api/config/arr", [this](const Req& req, Res& res) {
		try {
			auto b = json::parse(req.body);
			auto setVal = [&](const char* k, const std::string& v) {
				SQLite::Statement s(db_.get(),
					"INSERT INTO app_config (key, value) VALUES (?,?) "
					"ON CONFLICT(key) DO UPDATE SET value = excluded.value");
				s.bind(1, std::string(k)); s.bind(2, v); s.exec();
			};
			if (b.contains("sonarr_url"))     setVal("sonarr_url",     b["sonarr_url"]);
			if (b.contains("sonarr_api_key")) setVal("sonarr_api_key", b["sonarr_api_key"]);
			if (b.contains("radarr_url"))     setVal("radarr_url",     b["radarr_url"]);
			if (b.contains("radarr_api_key")) setVal("radarr_api_key", b["radarr_api_key"]);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/config/arr", e); route::err(res, 400, e.what());
		}
	});

	svr.Post("/api/arr/lookup", [this](const Req& req, Res& res) {
		try {
			auto b    = json::parse(req.body);
			auto type = b.value("type", "");
			auto svc  = ArrServiceFactory::make(type, db_);
			if (!svc) { route::err(res, 400, type.empty() ? "type required" : "arr service not configured"); return; }

			std::string term;
			if (type == "show")
				term = !b.value("tvdb_id","").empty() ? "tvdb:" + b["tvdb_id"].get<std::string>() : b.value("title","");
			if (type == "movie") {
				if (!b.value("tmdb_id","").empty())      term = "tmdb:" + b["tmdb_id"].get<std::string>();
				else if (!b.value("imdb_id","").empty()) term = "imdb:" + b["imdb_id"].get<std::string>();
				else                                     term = b.value("title","");
			}
			if (term.empty()) { route::err(res, 400, "title or external ID required"); return; }

			auto results = svc->lookup(term);
			json out = json::array();
			for (const auto& r : results) {
				out.push_back({
					{"title",        r.title},
					{"year",         r.year},
					{"external_id",  r.external_id},
					{"poster_url",   r.poster_url},
					{"already_added",r.already_added},
					{"add_data",     r.add_data},
				});
			}
			route::ok(res, out.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/arr/lookup", e); route::err(res, 400, e.what());
		}
	});

	svr.Get("/api/arr/options/:type", [this](const Req& req, Res& res) {
		auto svc = ArrServiceFactory::make(req.path_params.at("type"), db_);
		if (!svc) { route::err(res, 400, "arr service not configured"); return; }
		auto opts = svc->getOptions();
		json profiles = json::array();
		for (const auto& p : opts.quality_profiles)
			profiles.push_back({{"id", p.id}, {"name", p.name}});
		json folders = json::array();
		for (const auto& f : opts.root_folders)
			folders.push_back(f);
		route::ok(res, json{{"quality_profiles", profiles}, {"root_folders", folders}}.dump());
	});

	svr.Post("/api/arr/add", [this](const Req& req, Res& res) {
		try {
			auto b   = json::parse(req.body);
			auto svc = ArrServiceFactory::make(b.value("type", ""), db_);
			if (!svc) { route::err(res, 400, "arr service not configured"); return; }
			if (!b.contains("add_data")) { route::err(res, 400, "add_data required"); return; }
			if (!svc->addFromRequest(b)) { route::err(res, 502, "arr add failed"); return; }
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/arr/add", e); route::err(res, 400, e.what());
		}
	});
}
