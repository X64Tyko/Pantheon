#include "PlaybackService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../../conf/ConfStore.h"
#include "../../db/Database.h"
#include "../../db/WatchProgressRepository.h"
#include "../../db/ContentRepository.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ctime>
#include <vector>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

PlaybackService::PlaybackService(const ServiceContext& ctx) : db_(ctx.db), conf_(ctx.conf) {}

namespace {

bool validContentType(const std::string& t) { return t == "movie" || t == "episode"; }

} // namespace

void PlaybackService::registerRoutes(httplib::Server& svr) {

	// ── GET /api/playback/:content_type/:id ───────────────────────────────────
	// Internal — called by Hephaestus (not the browser) to resolve a library
	// item to a playable file. Exempted from auth in Router.cpp's isPublicPath,
	// same bucket as /now and /played.
	svr.Get("/api/playback/:content_type/:id", [this](const Req& req, Res& res) {
		auto content_type = req.path_params.at("content_type");
		auto id            = req.path_params.at("id");
		if (!validContentType(content_type)) { route::err(res, 400, "content_type must be movie or episode"); return; }

		try {
			// A linked special (episode.linked_movie_id set — see ScraperManager's
			// specials linking) has no file of its own; its file_path/duration_ms
			// must live-resolve through the movie it's linked to, never a stale
			// copy, so a re-scanned/moved movie file is always reflected here.
			const char* sql = content_type == "movie"
				? "SELECT file_path, duration_ms, title FROM movie WHERE movie_id = ?"
				: R"(
					SELECT COALESCE(m.file_path, e.file_path),
					       COALESCE(m.duration_ms, e.duration_ms),
					       e.title
					FROM episode e LEFT JOIN movie m ON m.movie_id = e.linked_movie_id
					WHERE e.episode_id = ?
				)";
			SQLite::Statement q(db_.get(), sql);
			q.bind(1, id);
			if (!q.executeStep()) { route::err(res, 404, "not found"); return; }

			auto file_path = q.getColumn(0).getString();
			if (file_path.empty()) { route::err(res, 404, "no file for this item"); return; }

			route::ok(res, json{
				{"file_path",   conf_.applyPathMap(file_path)},
				{"duration_ms", q.getColumn(1).getInt64()},
				{"title",       q.getColumn(2).getString()},
			}.dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/playback/:content_type/:id", e); route::err(res, 500, e.what());
		}
	});

	// ── GET /api/watch-progress ────────────────────────────────────────────────
	// Movies: one card per in-progress movie, same as always. Shows: one card
	// per show, from that show's most-recently-touched episode. If that
	// episode was completed (finished naturally, skipped, or auto-advanced
	// past via the player's Up Next), the show only stays in Continue
	// Watching when there's a next episode to resume into — surfaced as a
	// fresh, unstarted "up next" card (position_ms=0, up_next=true) rather
	// than the show just vanishing until the viewer manually starts it again.
	// Mirrors resolvePlayTarget.ts's client-side resume logic, computed here
	// once per show for the shelf instead of on demand.
	svr.Get("/api/watch-progress", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		int limit = 24;
		if (auto it = req.params.find("limit"); it != req.params.end()) {
			try { limit = std::clamp(std::stoi(it->second), 1, 100); } catch (...) {}
		}

		try {
			json out = json::array();

			// completed is the rewatch count (see WatchProgressRepository.h), not a
			// 0/1 flag that resets when a rewatch starts — "still in progress" is
			// instead read straight off position vs duration, which is correct
			// whether this is a first watch or a currently-mid rewatch of
			// something finished before.
			SQLite::Statement movies(db_.get(), R"SQL(
				SELECT content_id, position_ms, duration_ms, updated_at
				FROM watch_progress WHERE user_id = ? AND content_type = 'movie'
					AND duration_ms > 0 AND position_ms < duration_ms
			)SQL");
			movies.bind(1, user->user_id);
			while (movies.executeStep()) {
				auto content_id = movies.getColumn(0).getString();
				SQLite::Statement m(db_.get(), "SELECT title FROM movie WHERE movie_id = ?");
				m.bind(1, content_id);
				if (!m.executeStep()) continue; // stale reference (deleted item)

				json r;
				r["content_type"] = "movie";
				r["content_id"]   = content_id;
				r["position_ms"]  = movies.getColumn(1).getInt64();
				r["duration_ms"]  = movies.getColumn(2).getInt64();
				r["updated_at"]   = movies.getColumn(3).getInt64();
				r["title"]        = m.getColumn(0).getString();
				out.push_back(std::move(r));
			}

			SQLite::Statement showsQ(db_.get(), R"SQL(
				SELECT DISTINCT e.show_id
				FROM watch_progress wp JOIN episode e ON e.episode_id = wp.content_id
				WHERE wp.user_id = ? AND wp.content_type = 'episode'
			)SQL");
			showsQ.bind(1, user->user_id);
			std::vector<std::string> show_ids;
			while (showsQ.executeStep()) show_ids.push_back(showsQ.getColumn(0).getString());

			WatchProgressRepository wpRepo(db_);
			ContentRepository contentRepo(db_);
			for (auto& show_id : show_ids) {
				auto state = wpRepo.getLatestForShow(user->user_id, show_id);
				if (!state) continue;

				auto content_id  = state->content_id;
				auto position_ms = state->position_ms;
				auto duration_ms = state->duration_ms;
				bool up_next     = false;

				if (state->completed) {
					auto next = contentRepo.getNextEpisode(state->content_id);
					if (!next) continue; // finale watched — nothing to continue into
					content_id  = next->episode_id;
					position_ms = 0;
					duration_ms = next->duration_ms;
					up_next     = true;
				}

				SQLite::Statement e(db_.get(), "SELECT title, season, episode FROM episode WHERE episode_id = ?");
				e.bind(1, content_id);
				if (!e.executeStep()) continue;

				json r;
				r["content_type"] = "episode";
				r["content_id"]   = content_id;
				r["position_ms"]  = position_ms;
				r["duration_ms"]  = duration_ms;
				// Kept as the completed episode's timestamp (not "now") when
				// synthesizing an up_next card, so shelf ordering still
				// reflects how recently the show was actually watched.
				r["updated_at"]   = state->updated_at;
				r["title"]        = e.getColumn(0).getString();
				r["season"]       = e.getColumn(1).getInt();
				r["episode"]      = e.getColumn(2).getInt();
				r["show_id"]      = show_id;
				if (up_next) r["up_next"] = true;

				SQLite::Statement s(db_.get(), "SELECT title FROM show WHERE show_id = ?");
				s.bind(1, show_id);
				r["show_title"] = s.executeStep() ? s.getColumn(0).getString() : "";

				out.push_back(std::move(r));
			}

			std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
				return a["updated_at"].get<int64_t>() > b["updated_at"].get<int64_t>();
			});
			if (out.size() > static_cast<size_t>(limit)) out.erase(out.begin() + limit, out.end());

			route::ok(res, out.dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/watch-progress", e); route::err(res, 500, e.what());
		}
	});

	// ── PUT /api/watch-progress/:content_type/:id ─────────────────────────────
	// Upserts position. An item finished (>=95% through, or explicit
	// {"completed":true} — used by the player's skip-credits/up-next action to
	// mark an episode played before it's actually reached 95%) is stored with
	// completed=1 and position clamped to duration, rather than deleted, so
	// "played" survives as a durable fact (see migration v73). Ordinary pings
	// with no "completed" field behave exactly as before.
	svr.Put("/api/watch-progress/:content_type/:id", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		auto content_type = req.path_params.at("content_type");
		auto content_id    = req.path_params.at("id");
		if (!validContentType(content_type)) { route::err(res, 400, "content_type must be movie or episode"); return; }

		try {
			auto b = json::parse(req.body);
			int64_t position_ms = b.value("position_ms", int64_t{0});
			int64_t duration_ms = b.value("duration_ms", int64_t{0});
			if (position_ms < 0) position_ms = 0;

			bool completed = b.value("completed", false) ||
				(duration_ms > 0 && position_ms >= static_cast<int64_t>(duration_ms * 0.95));
			if (completed && duration_ms > 0) position_ms = duration_ms;

			WatchProgressRepository(db_).upsert(user->user_id, content_type, content_id,
				position_ms, duration_ms, static_cast<int64_t>(std::time(nullptr)), completed);

			route::ok(res, json{{"ok", true}, {"watched", completed}}.dump());
		} catch (const std::exception& e) {
			route::logErr("PUT /api/watch-progress/:content_type/:id", e); route::err(res, 400, e.what());
		}
	});

	// ── GET /api/shows/:id/watch-state ────────────────────────────────────────
	// The most-recently-touched episode watch_progress row for this show,
	// completed or not — lets the player distinguish "resume mid-episode"
	// from "the last episode was finished, continue at the next one" (see
	// hades/src/player/resolvePlayTarget.ts). Unlike the Continue Watching
	// list above, this deliberately does not filter out completed rows.
	svr.Get("/api/shows/:id/watch-state", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		try {
			auto state = WatchProgressRepository(db_).getLatestForShow(user->user_id, req.path_params.at("id"));
			if (!state) { route::ok(res, "null"); return; }

			route::ok(res, json{
				{"content_id",  state->content_id},
				{"position_ms", state->position_ms},
				{"duration_ms", state->duration_ms},
				{"completed",   state->completed},
				{"updated_at",  state->updated_at},
			}.dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/shows/:id/watch-state", e); route::err(res, 500, e.what());
		}
	});

	// ── DELETE /api/watch-progress/:content_type/:id ──────────────────────────
	svr.Delete("/api/watch-progress/:content_type/:id", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		auto content_type = req.path_params.at("content_type");
		auto content_id    = req.path_params.at("id");

		try {
			WatchProgressRepository(db_).remove(user->user_id, content_type, content_id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/watch-progress/:content_type/:id", e); route::err(res, 400, e.what());
		}
	});
}
