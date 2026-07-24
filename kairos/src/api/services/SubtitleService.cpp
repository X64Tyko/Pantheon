#include "SubtitleService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../ServiceContext.h"
#include "../../conf/ConfStore.h"
#include "../../db/Database.h"
#include "../../source/SubtitleValidation.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <set>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

SubtitleService::SubtitleService(const ServiceContext& ctx) : db_(ctx.db), conf_(ctx.conf), repo_(ctx.db) {}

namespace {
// Same convention as PlaybackService.cpp's own lookupTitle — a plain inline
// query rather than a shared repository method, since it's only ever needed
// by this one read path. Empty on any failure/missing row.
std::string lookupTitle(Database& db, const std::string& media_type, const std::string& media_id) {
	try {
		const char* sql = media_type == "movie"
			? "SELECT title FROM movie WHERE movie_id = ?"
			: "SELECT title FROM episode WHERE episode_id = ?";
		SQLite::Statement q(db.get(), sql);
		q.bind(1, media_id);
		if (q.executeStep()) return q.getColumn(0).getString();
	} catch (...) {}
	return "";
}

json subtitleTrackJson(Database& db, const SubtitleTrack& t) {
	return {
		{"subtitle_id",    t.subtitle_id},
		{"media_type",     t.media_type},
		{"media_id",       t.media_id},
		{"title",          lookupTitle(db, t.media_type, t.media_id)},
		{"file_path",      t.file_path},
		{"language",       t.language},
		{"forced",         t.forced},
		{"sdh",            t.sdh},
		{"source",         t.source},
		{"valid",          t.valid},
		{"invalid_reason", t.invalid_reason},
	};
}
} // namespace

void SubtitleService::registerRoutes(httplib::Server& svr) {

	// ── GET /api/subtitles/broken — admin review list ──────────────────────────
	svr.Get("/api/subtitles/broken", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json arr = json::array();
		for (const auto& t : repo_.listInvalid()) arr.push_back(subtitleTrackJson(db_, t));
		route::ok(res, arr.dump());
	});

	// ── POST /api/subtitles/:id/recheck — re-run validation now ────────────────
	// For after the admin has gone and fixed/replaced the file on disk,
	// rather than waiting for the next full library sync to notice.
	svr.Post("/api/subtitles/:id/recheck", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }

		auto existing = repo_.getById(req.path_params.at("id"));
		if (!existing) { route::err(res, 404, "not found"); return; }

		// existing->file_path is unmapped (like episode/movie.file_path) —
		// needs the real, readable host path to actually check the file.
		auto validation = validateSubtitleFile(conf_.applyPathMap(existing->file_path));
		repo_.updateValidity(existing->subtitle_id, validation.valid, validation.reason);
		existing->valid          = validation.valid;
		existing->invalid_reason = validation.reason;
		route::ok(res, subtitleTrackJson(db_, *existing).dump());
	});

	// ── PATCH /api/subtitles/:id — manual metadata correction ──────────────────
	// Not durable against a source='file' row — see
	// SubtitleTrackRepository::update's own comment: the next full sync
	// still unconditionally regenerates every sidecar-derived row for that
	// item from scratch. Hades surfaces that caveat in the UI rather than
	// silently letting an edit look permanent when it isn't.
	svr.Patch("/api/subtitles/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }

		auto existing = repo_.getById(req.path_params.at("id"));
		if (!existing) { route::err(res, 404, "not found"); return; }

		try {
			auto b = json::parse(req.body);
			std::string language = b.value("language", existing->language);
			bool forced = b.value("forced", existing->forced);
			bool sdh    = b.value("sdh", existing->sdh);
			repo_.update(existing->subtitle_id, language, forced, sdh);
			existing->language = language;
			existing->forced   = forced;
			existing->sdh      = sdh;
			route::ok(res, subtitleTrackJson(db_, *existing).dump());
		} catch (const std::exception& e) {
			route::err(res, 400, e.what());
		}
	});

	// ── DELETE /api/subtitles/:id ────────────────────────────────────────────
	// Deletes the actual sidecar file from the media library, not just this
	// DB row — the whole point is clearing the slot so a downloader like
	// Bazarr treats the language as missing again and fetches a fresh file.
	// Just dropping the row (the original implementation) left the broken
	// file in place, which is actively misleading (the row silently comes
	// back on the next sync since the file's still there and still fails
	// validation) and doesn't accomplish what "delete" implies. Permanent —
	// no trash/recycle bin — so this is gated on admin same as everything
	// else here, plus an extension allowlist below as a last line of defense
	// against ever unlinking something that isn't actually a subtitle file.
	svr.Delete("/api/subtitles/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto existing = repo_.getById(req.path_params.at("id"));
		if (!existing) { route::err(res, 404, "not found"); return; }

		auto mapped = conf_.applyPathMap(existing->file_path);
		std::string ext = std::filesystem::path(mapped).extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
		static const std::set<std::string> kSubtitleExts = {".srt", ".ass", ".ssa", ".vtt"};
		if (!kSubtitleExts.count(ext)) {
			route::err(res, 400, "refusing to delete a file with unexpected extension: " + ext);
			return;
		}

		std::error_code ec;
		bool existed = std::filesystem::exists(mapped, ec);
		if (existed) {
			std::filesystem::remove(mapped, ec);
			if (ec) {
				route::err(res, 500, "failed to delete file on disk: " + ec.message());
				return;
			}
		}
		// Row removed only after the file is actually gone (or was already
		// gone) — an admin re-checking this list should never see a "deleted"
		// entry vanish from the report while the real file is still sitting
		// there broken.
		repo_.remove(existing->subtitle_id);
		route::ok(res, json{{"ok", true}, {"file_deleted", existed}}.dump());
	});
}
