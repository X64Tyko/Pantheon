#include "PlaylistService.h"
#include "../RouteHelpers.h"
#include "PlexSyncHelper.h"
#include "../../db/Database.h"
#include "../../db/PlaylistRepository.h"
#include "../../source/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

PlaylistService::PlaylistService(const ServiceContext& ctx)
	: db_(ctx.db), sync_(ctx.sync) {}

void PlaylistService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/playlists", [this](const Req&, Res& res) {
		try {
			SQLite::Statement q(db_.get(), R"(
				SELECT p.playlist_id, p.title, p.mode,
				       COUNT(pi.id) AS item_count,
				       COALESCE(SUM(CASE pi.item_type
				           WHEN 'episode' THEN e.duration_ms
				           WHEN 'movie'   THEN m.duration_ms ELSE 0 END), 0) AS total_ms,
				       pll.source_id, pll.external_id, pll.plex_type, pll.last_synced_at
				FROM playlist p
				LEFT JOIN playlist_item pi ON pi.playlist_id = p.playlist_id
				LEFT JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
				LEFT JOIN movie   m ON pi.item_type = 'movie'   AND pi.item_id = m.movie_id
				LEFT JOIN plex_list_link pll ON pll.list_type = 'playlist' AND pll.list_id = p.playlist_id
				GROUP BY p.playlist_id ORDER BY p.title
			)");
			json result = json::array();
			while (q.executeStep()) {
				json entry = {
					{"playlist_id", q.getColumn(0).getString()},
					{"title",       q.getColumn(1).getString()},
					{"mode",        q.getColumn(2).getString()},
					{"item_count",  q.getColumn(3).getInt()},
					{"total_ms",    q.getColumn(4).getInt64()},
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
		} catch (const std::exception& e) { route::logErr("GET /api/playlists", e); route::err(res, 500, e.what()); }
	});

	// Must register before /:id routes
	svr.Post("/api/playlists/plex-sync-all", [this](const Req&, Res& res) {
		sync_.triggerPlexLinkSync();
		res.status = 202;
		route::ok(res, json{{"status","accepted"}}.dump());
	});

	svr.Post("/api/playlists", [this](const Req& req, Res& res) {
		try {
			auto b = json::parse(req.body);
			std::string title = b.value("title", "");
			if (title.empty()) { route::err(res, 400, "title required"); return; }
			auto playlist_id = PlaylistRepository(db_).create(title);
			res.status = 201;
			route::ok(res, json{{"playlist_id", playlist_id}}.dump());
		} catch (const std::exception& e) { route::logErr("POST /api/playlists", e); route::err(res, 400, e.what()); }
	});

	svr.Get("/api/playlists/:id", [this](const Req& req, Res& res) {
		try {
			auto id = req.path_params.at("id");
			SQLite::Statement ph(db_.get(),
				"SELECT playlist_id, title, mode FROM playlist WHERE playlist_id = ?");
			ph.bind(1, id);
			if (!ph.executeStep()) { route::err(res, 404, "playlist not found"); return; }

			SQLite::Statement q(db_.get(), R"(
				SELECT pi.id, pi.position, pi.item_type, pi.item_id,
				       CASE pi.item_type
				           WHEN 'episode' THEN s.title || ' S' || PRINTF('%02d',e.season) ||
				                               'E' || PRINTF('%02d',e.episode) || ' — ' || e.title
				           WHEN 'movie'   THEN m.title ELSE ''
				       END AS title,
				       CASE pi.item_type
				           WHEN 'episode' THEN e.duration_ms
				           WHEN 'movie'   THEN m.duration_ms ELSE 0
				       END AS duration_ms,
				       e.season, e.episode
				FROM playlist_item pi
				LEFT JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
				LEFT JOIN show    s ON e.show_id = s.show_id
				LEFT JOIN movie   m ON pi.item_type = 'movie'   AND pi.item_id = m.movie_id
				WHERE pi.playlist_id = ? ORDER BY pi.position
			)");
			q.bind(1, id);
			json items = json::array();
			while (q.executeStep()) {
				json item = {
					{"id",          q.getColumn(0).getInt()},
					{"position",    q.getColumn(1).getInt()},
					{"item_type",   q.getColumn(2).getString()},
					{"item_id",     q.getColumn(3).getString()},
					{"title",       q.getColumn(4).getString()},
					{"duration_ms", q.getColumn(5).getInt64()},
				};
				if (!q.getColumn(6).isNull()) {
					item["season"]  = q.getColumn(6).getInt();
					item["episode"] = q.getColumn(7).getInt();
				}
				items.push_back(item);
			}
			route::ok(res, json{
				{"playlist_id", ph.getColumn(0).getString()},
				{"title",       ph.getColumn(1).getString()},
				{"mode",        ph.getColumn(2).getString()},
				{"items",       items}}.dump());
		} catch (const std::exception& e) { route::logErr("GET /api/playlists/:id", e); route::err(res, 500, e.what()); }
	});

	svr.Post("/api/playlists/:id/plex-sync", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			auto b           = json::parse(req.body);
			std::string src  = b.value("source_id",   "");
			std::string ext  = b.value("external_id", "");
			std::string kind = b.value("plex_type",   "");
			if (src.empty() || ext.empty() || (kind != "playlist" && kind != "collection")) {
				route::err(res, 400, "source_id, external_id, plex_type required"); return;
			}
			syncPlexListItems(res, "playlist", id, src, ext, kind, db_, sync_);
		} catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/playlists/:id/plex-link", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			PlaylistRepository(db_).unlinkPlex(id);
			res.status = 204;
			res.set_content("", "application/json");
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/playlists/:id/plex-link", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Patch("/api/playlists/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			PlaylistRepository repo(db_);
			if (b.contains("title")) repo.updateField(id, "title", b["title"].get<std::string>());
			if (b.contains("mode")) {
				std::string mode = b["mode"].get<std::string>();
				if (mode != "sequential" && mode != "show_collection") {
					route::err(res, 400, "mode must be 'sequential' or 'show_collection'"); return;
				}
				repo.updateField(id, "mode", mode);
			}
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("PATCH /api/playlists/:id", e); route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/playlists/:id", [this](const Req& req, Res& res) {
		auto id = req.path_params.at("id");
		try {
			PlaylistRepository(db_).remove(id);
			route::ok(res, json{{"deleted", id}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/playlists/:id", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/playlists/:id/items", [this](const Req& req, Res& res) {
		auto playlist_id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::string item_type = b.value("item_type", "episode");
			std::string item_id   = b.value("item_id",   "");
			if (item_id.empty()) { route::err(res, 400, "item_id required"); return; }
			auto [id, position] = PlaylistRepository(db_).addItem(playlist_id, item_type, item_id);
			res.status = 201;
			route::ok(res, json{{"id", id}, {"position", position}}.dump());
		} catch (const SQLite::Exception& e) { route::logErr("POST /api/playlists/:id/items", e); route::err(res, 409, e.what()); }
		  catch (const std::exception& e)    { route::logErr("POST /api/playlists/:id/items", e); route::err(res, 400, e.what()); }
	});

	svr.Post("/api/playlists/:id/items/bulk", [this](const Req& req, Res& res) {
		auto playlist_id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::vector<std::pair<std::string, std::string>> items;
			for (const auto& item : b.at("items"))
				items.emplace_back(item.value("item_type", "episode"), item.value("item_id", ""));
			int added = PlaylistRepository(db_).addItems(playlist_id, items);
			route::ok(res, json{{"added", added}}.dump());
		} catch (const std::exception& e) { route::logErr("POST /api/playlists/:id/items/bulk", e); route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/playlists/:id/items/:iid", [this](const Req& req, Res& res) {
		auto playlist_id = req.path_params.at("id");
		auto iid         = std::stoi(req.path_params.at("iid"));
		try {
			PlaylistRepository(db_).removeItem(iid, playlist_id);
			route::ok(res, json{{"deleted", iid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/playlists/:id/items/:iid", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Patch("/api/playlists/:id/items/:iid", [this](const Req& req, Res& res) {
		auto playlist_id = req.path_params.at("id");
		auto iid         = std::stoi(req.path_params.at("iid"));
		try {
			auto b = json::parse(req.body);
			if (b.contains("position"))
				PlaylistRepository(db_).moveItem(iid, playlist_id, b["position"].get<int>());
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("PATCH /api/playlists/:id/items/:iid", e); route::err(res, 400, e.what()); }
	});
}
