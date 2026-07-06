#include "ChapterService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../ServiceContext.h"
#include "../../db/SourceRepository.h"
#include "../../db/ContentRepository.h"
#include "../../detect/ChapterDetectionManager.h"
#include "../../source/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <string>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

ChapterService::ChapterService(const ServiceContext& ctx, ChapterDetectionManager& detector)
	: db_(ctx.db), sync_(ctx.sync), repo_(ctx.db), detector_(detector) {}

void ChapterService::registerRoutes(httplib::Server& svr) {

	// ── Review browsing (admin, read-only) ────────────────────────────────────

	svr.Get("/api/chapters/review", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }

		std::string media_type   = req.has_param("media_type")   ? req.get_param_value("media_type")   : "all";
		std::string chapter_type = req.has_param("chapter_type") ? req.get_param_value("chapter_type") : "all";
		std::string q            = req.has_param("q")             ? req.get_param_value("q")            : "";
		int limit = 40, offset = 0;
		if (req.has_param("limit"))  { try { limit  = std::stoi(req.get_param_value("limit"));  } catch (...) {} }
		if (req.has_param("offset")) { try { offset = std::stoi(req.get_param_value("offset")); } catch (...) {} }

		auto result = repo_.listReviewItems(media_type, chapter_type, q, limit, offset);
		json arr = json::array();
		for (const auto& it : result.items) {
			json ij;
			ij["media_type"]  = it.media_type;
			ij["media_id"]    = it.media_id;
			ij["title"]       = it.title;
			ij["thumb"]       = it.thumb;
			ij["file_path"]   = it.file_path;
			ij["duration_ms"] = it.duration_ms;
			if (it.year > 0) ij["year"] = it.year;
			ij["chapters"]    = ChapterRepository::toJson(it.chapters);
			arr.push_back(std::move(ij));
		}
		route::ok(res, json{{"items", arr}, {"total", result.total}}.dump());
	});

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
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
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
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		if (sync_.isMediaLocked()) { route::err(res, 423, "sync in progress"); return; }
		json body;
		try { body = json::parse(req.body); }
		catch (...) { route::err(res, 400, "invalid JSON"); return; }
		repo_.update(req.path_params.at("id"), body);
		route::ok(res, "{}");
	});

	// ── Delete ────────────────────────────────────────────────────────────────

	svr.Delete("/api/chapters/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		if (sync_.isMediaLocked()) { route::err(res, 423, "sync in progress"); return; }
		repo_.remove(req.path_params.at("id"));
		res.status = 204;
	});

	// ── Per-item sync (all sources) ───────────────────────────────────────────

	svr.Post("/api/episodes/:id/chapters/sync", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		const std::string kairos_id = req.path_params.at("id");
		auto resolved = SourceRepository(db_).resolveItemSource("episode", kairos_id);
		if (!resolved) { route::err(res, 404, "episode not found"); return; }
		auto* src = sync_.findSource(resolved->source_id);
		if (!src) { route::err(res, 422, "source not loaded"); return; }
		sync_.syncItemChapters(*src, "episode", kairos_id, resolved->external_id, resolved->file_path);
		route::ok(res, ChapterRepository::toJson(
		    repo_.get("episode", kairos_id)).dump());
	});

	svr.Post("/api/movies/:id/chapters/sync", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		const std::string kairos_id = req.path_params.at("id");
		auto resolved = SourceRepository(db_).resolveItemSource("movie", kairos_id);
		if (!resolved) { route::err(res, 404, "movie not found"); return; }
		auto* src = sync_.findSource(resolved->source_id);
		if (!src) { route::err(res, 422, "source not loaded"); return; }
		sync_.syncItemChapters(*src, "movie", kairos_id, resolved->external_id, resolved->file_path);
		route::ok(res, ChapterRepository::toJson(
		    repo_.get("movie", kairos_id)).dump());
	});

	// Show-scoped: loops every episode through the same resolve+sync path as the per-episode route above.
	svr.Post("/api/shows/:id/chapters/sync", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		const std::string show_id = req.path_params.at("id");
		auto episodes = ContentRepository(db_).listEpisodesForShow(show_id, "");
		if (episodes.empty()) { route::err(res, 404, "show not found or has no episodes"); return; }

		SourceRepository src_repo(db_);
		int processed = 0, with_chapters = 0;
		for (const auto& ep : episodes) {
			auto resolved = src_repo.resolveItemSource("episode", ep.episode_id);
			if (!resolved) continue;
			auto* src = sync_.findSource(resolved->source_id);
			if (!src) continue;
			sync_.syncItemChapters(*src, "episode", ep.episode_id, resolved->external_id, resolved->file_path);
			++processed;
			if (!repo_.get("episode", ep.episode_id).empty()) ++with_chapters;
		}
		route::ok(res, json{
			{"episode_count", static_cast<int>(episodes.size())},
			{"processed",     processed},
			{"with_chapters", with_chapters},
		}.dump());
	});

	// ── Structure detection (admin, async) ────────────────────────────────────
	// Slow algorithmic analysis (ffmpeg scene-cut / audio fingerprinting), as
	// opposed to the fast marker re-probe the routes above run. See
	// detect/ChapterDetectionManager.h.

	svr.Post("/api/shows/:id/chapters/detect", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		if (sync_.isMediaLocked()) { route::err(res, 423, "sync in progress"); return; }
		const std::string show_id = req.path_params.at("id");
		if (ContentRepository(db_).listEpisodesForShow(show_id, "").empty()) {
			route::err(res, 404, "show not found or has no episodes"); return;
		}
		res.status = 202;
		route::ok(res, json{{"status", detector_.triggerShowDetect(show_id) ? "started" : "already_running"}}.dump());
	});

	svr.Post("/api/movies/:id/chapters/detect", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		if (sync_.isMediaLocked()) { route::err(res, 423, "sync in progress"); return; }
		const std::string movie_id = req.path_params.at("id");
		if (!ContentRepository(db_).getMovieDetail(movie_id)) { route::err(res, 404, "movie not found"); return; }
		res.status = 202;
		route::ok(res, json{{"status", detector_.triggerMovieDetect(movie_id) ? "started" : "already_running"}}.dump());
	});

	svr.Get("/api/chapters/detect/status", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		route::ok(res, json{{"running", detector_.isDetecting()}}.dump());
	});

	// ── Writeback stub ────────────────────────────────────────────────────────

	svr.Post("/api/chapters/:id/writeback", [](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		route::err(res, 501, "writeback not yet implemented");
	});
}
