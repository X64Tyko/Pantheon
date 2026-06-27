#include "ChapterService.h"
#include "../RouteHelpers.h"
#include "../ServiceContext.h"
#include "../../db/Database.h"
#include "../../source/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <string>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

ChapterService::ChapterService(const ServiceContext& ctx)
	: db_(ctx.db), sync_(ctx.sync), repo_(ctx.db) {}

void ChapterService::registerRoutes(httplib::Server& svr) {

	// ── List ─────────────────────────────────────────────────────────────────

	svr.Get("/api/episodes/:id/chapters", [this](const Req& req, Res& res) {
		auto chapters = repo_.get("episode", req.path_params.at("id"));
		route::ok(res, ChapterRepository::toJson(chapters).dump());
	});

	svr.Get("/api/movies/:id/chapters", [this](const Req& req, Res& res) {
		auto chapters = repo_.get("movie", req.path_params.at("id"));
		route::ok(res, ChapterRepository::toJson(chapters).dump());
	});

	// ── Create (manual) ──────────────────────────────────────────────────────

	svr.Post("/api/episodes/:id/chapters", [this](const Req& req, Res& res) {
		json body;
		try { body = json::parse(req.body); }
		catch (...) { route::err(res, 400, "invalid JSON"); return; }
		const std::string media_id = req.path_params.at("id");
		const std::string id = repo_.create("episode", media_id, body);
		res.status = 201;
		route::ok(res, ChapterRepository::toJson(
		    repo_.get("episode", media_id)).dump());
	});

	svr.Post("/api/movies/:id/chapters", [this](const Req& req, Res& res) {
		json body;
		try { body = json::parse(req.body); }
		catch (...) { route::err(res, 400, "invalid JSON"); return; }
		const std::string media_id = req.path_params.at("id");
		repo_.create("movie", media_id, body);
		res.status = 201;
		route::ok(res, ChapterRepository::toJson(
		    repo_.get("movie", media_id)).dump());
	});

	// ── Update ────────────────────────────────────────────────────────────────

	svr.Patch("/api/chapters/:id", [this](const Req& req, Res& res) {
		json body;
		try { body = json::parse(req.body); }
		catch (...) { route::err(res, 400, "invalid JSON"); return; }
		repo_.update(req.path_params.at("id"), body);
		route::ok(res, "{}");
	});

	// ── Delete ────────────────────────────────────────────────────────────────

	svr.Delete("/api/chapters/:id", [this](const Req& req, Res& res) {
		repo_.remove(req.path_params.at("id"));
		res.status = 204;
	});

	// ── Per-item sync (all sources) ───────────────────────────────────────────

	svr.Post("/api/episodes/:id/chapters/sync", [this](const Req& req, Res& res) {
		const std::string kairos_id = req.path_params.at("id");
		SQLite::Statement q(db_.get(), R"(
			SELECT sm.source_id, sm.external_id, e.file_path
			FROM source_mapping sm
			JOIN episode e ON e.episode_id = sm.kairos_id
			WHERE sm.item_type='episode' AND sm.kairos_id=?
			LIMIT 1
		)");
		q.bind(1, kairos_id);
		if (!q.executeStep()) { route::err(res, 404, "episode not found"); return; }
		const std::string source_id   = q.getColumn(0).getString();
		const std::string external_id = q.getColumn(1).getString();
		const std::string file_path   = q.getColumn(2).getString();
		auto* src = sync_.findSource(source_id);
		if (!src) { route::err(res, 422, "source not loaded"); return; }
		sync_.syncItemChapters(*src, "episode", kairos_id, external_id, file_path);
		route::ok(res, ChapterRepository::toJson(
		    repo_.get("episode", kairos_id)).dump());
	});

	svr.Post("/api/movies/:id/chapters/sync", [this](const Req& req, Res& res) {
		const std::string kairos_id = req.path_params.at("id");
		SQLite::Statement q(db_.get(), R"(
			SELECT sm.source_id, sm.external_id, m.file_path
			FROM source_mapping sm
			JOIN movie m ON m.movie_id = sm.kairos_id
			WHERE sm.item_type='movie' AND sm.kairos_id=?
			LIMIT 1
		)");
		q.bind(1, kairos_id);
		if (!q.executeStep()) { route::err(res, 404, "movie not found"); return; }
		const std::string source_id   = q.getColumn(0).getString();
		const std::string external_id = q.getColumn(1).getString();
		const std::string file_path   = q.getColumn(2).getString();
		auto* src = sync_.findSource(source_id);
		if (!src) { route::err(res, 422, "source not loaded"); return; }
		sync_.syncItemChapters(*src, "movie", kairos_id, external_id, file_path);
		route::ok(res, ChapterRepository::toJson(
		    repo_.get("movie", kairos_id)).dump());
	});

	// ── Writeback stub ────────────────────────────────────────────────────────

	svr.Post("/api/chapters/:id/writeback", [](const Req&, Res& res) {
		route::err(res, 501, "writeback not yet implemented");
	});
}
