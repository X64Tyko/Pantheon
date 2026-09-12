#include "FillerService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../ScheduleCache.h"
#include "PlexSyncHelper.h"
#include "../../db/Database.h"
#include "../../db/FillerRepository.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include "../../source/SyncManager.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

FillerService::FillerService(const ServiceContext& ctx)
	: db_(ctx.db), sync_(ctx.sync), schedule_cache_(ctx.schedule_cache) {}

// A filler_list isn't scoped to one channel — it can be referenced directly by a
// channel's own filler pool, by a block's filler pool, or as regular block content
// (content_type='filler_list'). Editing its items has to invalidate every channel
// that pulls from it, not just "the" channel, because there isn't one.
void FillerService::clearChannelsUsingFillerList(const std::string& filler_list_id) {
	SQLite::Statement q(db_.get(), R"(
		SELECT channel_id FROM channel_filler_entry WHERE filler_list_id = ?
		UNION
		SELECT b.channel_id FROM block_filler_entry bfe
			JOIN block b ON b.block_id = bfe.block_id WHERE bfe.filler_list_id = ?
		UNION
		SELECT b.channel_id FROM block_content bc
			JOIN block b ON b.block_id = bc.block_id
			WHERE bc.content_type = 'filler_list' AND bc.content_id = ?
	)");
	q.bind(1, filler_list_id);
	q.bind(2, filler_list_id);
	q.bind(3, filler_list_id);
	while (q.executeStep())
		schedule_cache_.clear(q.getColumn(0).getString());
}

void FillerService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/filler-lists", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			json result = json::array();
			for (const auto& r : FillerRepository(db_).listAll()) {
				json entry = {
					{"filler_list_id", r.filler_list_id},
					{"title",          r.title},
					{"advancement",    r.advancement},
					{"item_count",     r.item_count},
					{"total_ms",       r.total_ms},
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
		} catch (const std::exception& e) { route::logErr("GET /api/filler-lists", e); route::err(res, 500, e.what()); }
	});

	// Must register before /:id routes
	svr.Post("/api/filler-lists/plex-sync-all", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		sync_.triggerPlexLinkSync();
		res.status = 202;
		route::ok(res, json{{"status","accepted"}}.dump());
	});

	svr.Post("/api/filler-lists/source-sync-all", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		sync_.triggerPlexLinkSync();
		res.status = 202;
		route::ok(res, json{{"status","accepted"}}.dump());
	});

	svr.Post("/api/filler-lists", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			auto id = req.path_params.at("id");
			auto d = FillerRepository(db_).getDetail(id);
			if (!d) { route::err(res, 404, "filler list not found"); return; }
			json items = json::array();
			for (const auto& r : d->items) {
				items.push_back({
					{"id",          r.id},
					{"item_type",   r.item_type},
					{"item_id",     r.item_id},
					{"position",    r.position},
					{"title",       r.title},
					{"duration_ms", r.duration_ms},
				});
			}
			route::ok(res, json{
				{"filler_list_id", d->filler_list_id},
				{"title",          d->title},
				{"advancement",    d->advancement},
				{"items",          items}}.dump());
		} catch (const std::exception& e) { route::logErr("GET /api/filler-lists/:id", e); route::err(res, 500, e.what()); }
	});

	svr.Post("/api/filler-lists/:id/plex-sync", [this](const Req& req, Res& res) {
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
			syncPlexListItems(res, "filler_list", id, src, ext, kind, db_, sync_);
			clearChannelsUsingFillerList(id);
		} catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	svr.Post("/api/filler-lists/:id/source-sync", [this](const Req& req, Res& res) {
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
			syncSourceListItems(res, "filler_list", id, src, ext, kind, db_, sync_);
			clearChannelsUsingFillerList(id);
		} catch (const std::exception& e) { route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/filler-lists/:id/plex-link", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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

	svr.Delete("/api/filler-lists/:id/source-link", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			FillerRepository(db_).unlinkPlex(id);
			res.status = 204;
			res.set_content("", "application/json");
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/filler-lists/:id/source-link", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Patch("/api/filler-lists/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			FillerRepository repo(db_);
			if (b.contains("title"))       repo.updateField(id, "title",       b["title"].get<std::string>());
			if (b.contains("advancement")) {
				repo.updateField(id, "advancement", b["advancement"].get<std::string>());
				clearChannelsUsingFillerList(id); // playback order/style changed
			}
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) { route::logErr("PATCH /api/filler-lists/:id", e); route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/filler-lists/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			// Capture affected channels before remove() — its ON DELETE CASCADE
			// wipes the referencing rows this query depends on.
			clearChannelsUsingFillerList(id);
			FillerRepository(db_).remove(id);
			route::ok(res, json{{"deleted", id}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/filler-lists/:id", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/filler-lists/:id/items", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto fid = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::string item_type = b.value("item_type", "episode");
			std::string item_id   = b.value("item_id",   "");
			if (item_id.empty()) { route::err(res, 400, "item_id required"); return; }
			auto [id, position] = FillerRepository(db_).addItem(fid, item_type, item_id);
			clearChannelsUsingFillerList(fid);
			res.status = 201;
			route::ok(res, json{{"id", id}, {"position", position}}.dump());
		} catch (const SQLite::Exception& e) { route::logErr("POST /api/filler-lists/:id/items", e); route::err(res, 409, e.what()); }
		  catch (const std::exception& e)    { route::logErr("POST /api/filler-lists/:id/items", e); route::err(res, 400, e.what()); }
	});

	svr.Delete("/api/filler-lists/:id/items/:iid", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto fid = req.path_params.at("id");
		auto iid = std::stoi(req.path_params.at("iid"));
		try {
			FillerRepository(db_).removeItem(iid);
			clearChannelsUsingFillerList(fid);
			route::ok(res, json{{"deleted", iid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/filler-lists/:id/items/:iid", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/filler-lists/:id/items/bulk", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto fid = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			std::vector<std::pair<std::string, std::string>> items;
			for (const auto& item : b.at("items"))
				items.emplace_back(item.value("item_type", "episode"), item.value("item_id", ""));
			int added = FillerRepository(db_).addItems(fid, items);
			clearChannelsUsingFillerList(fid);
			route::ok(res, json{{"added", added}}.dump());
		} catch (const std::exception& e) { route::logErr("POST /api/filler-lists/:id/items/bulk", e); route::err(res, 400, e.what()); }
	});
}
