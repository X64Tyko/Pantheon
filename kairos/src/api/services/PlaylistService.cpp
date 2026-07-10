#include "PlaylistService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "PlexSyncHelper.h"
#include "../../db/PlaylistRepository.h"
#include "../../db/PlaylistSerializer.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include "../../source/SyncManager.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

PlaylistService::PlaylistService(const ServiceContext& ctx)
	: db_(ctx.db), sync_(ctx.sync) {}

void PlaylistService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/playlists", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			json result = json::array();
			for (const auto& r : PlaylistRepository(db_).listAll()) {
				json entry = {
					{"playlist_id", r.playlist_id},
					{"title",       r.title},
					{"mode",        r.mode},
					{"item_count",  r.item_count},
					{"total_ms",    r.total_ms},
				};
				if (r.plex_link) {
					entry["plex_link"] = {
						{"source_id",      r.plex_link->source_id},
						{"external_id",    r.plex_link->external_id},
						{"plex_type",      r.plex_link->plex_type},
						{"last_synced_at", r.plex_link->last_synced_at
							? json(*r.plex_link->last_synced_at) : json(nullptr)},
					};
				}
				result.push_back(entry);
			}
			route::ok(res, result.dump());
		} catch (const std::exception& e) { route::logErr("GET /api/playlists", e); route::err(res, 500, e.what()); }
	});

	// Must register before /:id routes
	svr.Post("/api/playlists/plex-sync-all", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		sync_.triggerPlexLinkSync();
		res.status = 202;
		route::ok(res, json{{"status","accepted"}}.dump());
	});

	svr.Post("/api/playlists/source-sync-all", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		sync_.triggerPlexLinkSync();
		res.status = 202;
		route::ok(res, json{{"status","accepted"}}.dump());
	});

	svr.Post("/api/playlists", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			auto id = req.path_params.at("id");
			auto d = PlaylistRepository(db_).getDetail(id);
			if (!d) { route::err(res, 404, "playlist not found"); return; }
			json items = json::array();
			for (const auto& r : d->items) {
				json item = {
					{"id",          r.id},
					{"position",    r.position},
					{"item_type",   r.item_type},
					{"item_id",     r.item_id},
					{"title",       r.title},
					{"duration_ms", r.duration_ms},
				};
				if (r.season)  item["season"]  = *r.season;
				if (r.episode) item["episode"] = *r.episode;
				items.push_back(item);
			}
			route::ok(res, json{
				{"playlist_id", d->playlist_id},
				{"title",       d->title},
				{"mode",        d->mode},
				{"items",       items}}.dump());
		} catch (const std::exception& e) { route::logErr("GET /api/playlists/:id", e); route::err(res, 500, e.what()); }
	});

	// Portable JSON export/import — same pattern as channel export/import, lets
	// a playlist be shared/moved between Pantheon instances (or people).
	// Must register the literal "import"/"import/preview" paths before any
	// bare POST /api/playlists/:id/... pattern that could otherwise swallow
	// them — there isn't one today, but keeping the same registration-order
	// discipline ChannelService follows.
	svr.Get("/api/playlists/:id/export", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id   = req.path_params.at("id");
		bool deep = req.has_param("deep") && req.get_param_value("deep") == "true";
		try {
			route::ok(res, PlaylistSerializer(db_).exportPlaylist(id, deep).dump(2));
		} catch (const std::exception& e) {
			route::logErr("GET /api/playlists/:id/export", e);
			route::err(res, 404, e.what());
		}
	});

	svr.Post("/api/playlists/import/preview", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			auto body = json::parse(req.body);
			bool deep = body.value("depth", "shallow") == "deep";
			route::ok(res, PlaylistSerializer(db_).previewImport(body, deep).dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/playlists/import/preview", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Post("/api/playlists/import", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			auto body = json::parse(req.body);
			if (!body.value("kairos_export", 0)) { route::err(res, 400, "not a valid Kairos playlist export"); return; }
			bool deep = body.value("depth", "shallow") == "deep";
			auto [playlist_id, unresolved] = PlaylistSerializer(db_).importPlaylist(body, deep);
			res.status = 201;
			route::ok(res, json{{"playlist_id", playlist_id}, {"unresolved", unresolved}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/playlists/import", e);
			route::err(res, 400, e.what());
		}
	});

	svr.Post("/api/playlists/:id/plex-sync", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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

	svr.Post("/api/playlists/:id/source-sync", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b           = json::parse(req.body);
			std::string src  = b.value("source_id",   "");
			std::string ext  = b.value("external_id", "");
			std::string kind = b.value("list_kind",   "");
			if (src.empty() || ext.empty() || (kind != "playlist" && kind != "collection")) {
				route::err(res, 400, "source_id, external_id, list_kind required"); return;
			}
			syncSourceListItems(res, "playlist", id, src, ext, kind, db_, sync_);
		} catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/playlists/:id/plex-link", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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

	svr.Delete("/api/playlists/:id/source-link", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			PlaylistRepository(db_).unlinkPlex(id);
			res.status = 204;
			res.set_content("", "application/json");
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/playlists/:id/source-link", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Patch("/api/playlists/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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
