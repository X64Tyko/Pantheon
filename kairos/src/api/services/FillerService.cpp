#include "FillerService.h"
#include "../RouteHelpers.h"
#include "PlexSyncHelper.h"
#include "../../db/Database.h"
#include "../../db/FillerRepository.h"
#include "../../source/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

FillerService::FillerService(const ServiceContext& ctx)
	: db_(ctx.db), sync_(ctx.sync) {}

void FillerService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/filler-lists", [this](const Req&, Res& res) {
		try {
			SQLite::Statement q(db_.get(), R"(
				SELECT fl.filler_list_id, fl.title, fl.advancement,
				       COUNT(fi.id) AS item_count,
				       COALESCE(SUM(CASE fi.item_type
				           WHEN 'episode' THEN e.duration_ms
				           WHEN 'movie'   THEN m.duration_ms ELSE 0 END), 0) AS total_ms,
				       pll.source_id, pll.external_id, pll.plex_type, pll.last_synced_at
				FROM filler_list fl
				LEFT JOIN filler_list_item fi ON fi.filler_list_id = fl.filler_list_id
				LEFT JOIN episode e ON fi.item_type = 'episode' AND fi.item_id = e.episode_id
				LEFT JOIN movie   m ON fi.item_type = 'movie'   AND fi.item_id = m.movie_id
				LEFT JOIN plex_list_link pll ON pll.list_type = 'filler_list' AND pll.list_id = fl.filler_list_id
				GROUP BY fl.filler_list_id ORDER BY fl.title
			)");
			json result = json::array();
			while (q.executeStep()) {
				json entry = {
					{"filler_list_id", q.getColumn(0).getString()},
					{"title",          q.getColumn(1).getString()},
					{"advancement",    q.getColumn(2).getString()},
					{"item_count",     q.getColumn(3).getInt()},
					{"total_ms",       q.getColumn(4).getInt64()},
				};
				if (!q.getColumn(5).isNull()) {
					entry["plex_link"] = {
						{"source_id",      q.getColumn(5).getString()},
						{"external_id",    q.getColumn(6).getString()},
						{"plex_type",      q.getColumn(7).getString()},
						{"last_synced_at", q.getColumn(8).isNull()
							? json(nullptr) : json(q.getColumn(8).getInt64())},
					};
				}
				result.push_back(entry);
			}
			route::ok(res, result.dump());
		} catch (const std::exception& e) { route::logErr("GET /api/filler-lists", e); route::err(res, 500, e.what()); }
	});

	// Must register before /:id routes
	svr.Post("/api/filler-lists/plex-sync-all", [this](const Req&, Res& res) {
		sync_.triggerPlexLinkSync();
		res.status = 202;
		route::ok(res, json{{"status","accepted"}}.dump());
	});

	svr.Post("/api/filler-lists", [this](const Req& req, Res& res) {
		try {
			auto b = json::parse(req.body);
			std::string title       = b.value("title",       "");
			std::string advancement = b.value("advancement", "shuffle");
			if (title.empty()) { route::err(res, 400, "title required"); return; }
			auto fid = FillerRepository(db_).create(title, advancement);
			res.status = 201;
			route::ok(res, json{{"filler_list_id", fid}}.dump());
		} catch (const std::exception& e) { route::logErr("POST /api/filler-lists", e); route::err(res, 400, e.what()); }
	});

	svr.Get("/api/filler-lists/:id", [this](const Req& req, Res& res) {
		try {
			auto id = req.path_params.at("id");
			SQLite::Statement fh(db_.get(),
				"SELECT filler_list_id, title, advancement FROM filler_list WHERE filler_list_id = ?");
			fh.bind(1, id);
			if (!fh.executeStep()) { route::err(res, 404, "filler list not found"); return; }

			SQLite::Statement q(db_.get(), R"(
				SELECT fi.id, fi.item_type, fi.item_id, fi.position,
				       CASE fi.item_type
				           WHEN 'episode' THEN s.title || ' S' || PRINTF('%02d',e.season) ||
				                               'E' || PRINTF('%02d',e.episode) || ' — ' || e.title
				           WHEN 'movie'   THEN m.title ELSE ''
				       END AS title,
				       CASE fi.item_type
				           WHEN 'episode' THEN e.duration_ms
				           WHEN 'movie'   THEN m.duration_ms ELSE 0
				       END AS duration_ms
				FROM filler_list_item fi
				LEFT JOIN episode e ON fi.item_type = 'episode' AND fi.item_id = e.episode_id
				LEFT JOIN show    s ON e.show_id = s.show_id
				LEFT JOIN movie   m ON fi.item_type = 'movie'   AND fi.item_id = m.movie_id
				WHERE fi.filler_list_id = ? ORDER BY fi.position
			)");
			q.bind(1, id);
			json items = json::array();
			while (q.executeStep()) {
				items.push_back({
					{"id",          q.getColumn(0).getInt()},
					{"item_type",   q.getColumn(1).getString()},
					{"item_id",     q.getColumn(2).getString()},
					{"position",    q.getColumn(3).getInt()},
					{"title",       q.getColumn(4).getString()},
					{"duration_ms", q.getColumn(5).getInt64()},
				});
			}
			route::ok(res, json{
				{"filler_list_id", fh.getColumn(0).getString()},
				{"title",          fh.getColumn(1).getString()},
				{"advancement",    fh.getColumn(2).getString()},
				{"items",          items}}.dump());
		} catch (const std::exception& e) { route::logErr("GET /api/filler-lists/:id", e); route::err(res, 500, e.what()); }
	});

	svr.Post("/api/filler-lists/:id/plex-sync", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			auto b           = json::parse(req.body);
			std::string src  = b.value("source_id",   "");
			std::string ext  = b.value("external_id", "");
			std::string kind = b.value("plex_type",   "");
			if (src.empty() || ext.empty() || (kind != "playlist" && kind != "collection")) {
				route::err(res, 400, "source_id, external_id, plex_type required"); return;
			}
			syncPlexListItems(res, "filler_list", id, src, ext, kind, db_, sync_);
		} catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/filler-lists/:id/plex-link", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			FillerRepository(db_).unlinkPlex(id);
			res.status = 204;
			res.set_content("", "application/json");
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/filler-lists/:id/plex-link", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Patch("/api/filler-lists/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			FillerRepository repo(db_);
			if (b.contains("title"))       repo.updateField(id, "title",       b["title"].get<std::string>());
			if (b.contains("advancement")) repo.updateField(id, "advancement", b["advancement"].get<std::string>());
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("PATCH /api/filler-lists/:id", e); route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/filler-lists/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			FillerRepository(db_).remove(id);
			route::ok(res, json{{"deleted", id}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/filler-lists/:id", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/filler-lists/:id/items", [this](const Req& req, Res& res) {
		auto fid = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::string item_type = b.value("item_type", "episode");
			std::string item_id   = b.value("item_id",   "");
			if (item_id.empty()) { route::err(res, 400, "item_id required"); return; }
			auto [id, position] = FillerRepository(db_).addItem(fid, item_type, item_id);
			res.status = 201;
			route::ok(res, json{{"id", id}, {"position", position}}.dump());
		} catch (const SQLite::Exception& e) { route::logErr("POST /api/filler-lists/:id/items", e); route::err(res, 409, e.what()); }
		  catch (const std::exception& e)    { route::logErr("POST /api/filler-lists/:id/items", e); route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/filler-lists/:id/items/:iid", [this](const Req& req, Res& res) {
		auto iid = std::stoi(req.path_params.at("iid"));
		try {
			FillerRepository(db_).removeItem(iid);
			route::ok(res, json{{"deleted", iid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/filler-lists/:id/items/:iid", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/filler-lists/:id/items/bulk", [this](const Req& req, Res& res) {
		auto fid = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::vector<std::pair<std::string, std::string>> items;
			for (const auto& item : b.at("items"))
				items.emplace_back(item.value("item_type", "episode"), item.value("item_id", ""));
			int added = FillerRepository(db_).addItems(fid, items);
			route::ok(res, json{{"added", added}}.dump());
		} catch (const std::exception& e) { route::logErr("POST /api/filler-lists/:id/items/bulk", e); route::err(res, 400, e.what()); }
	});
}
