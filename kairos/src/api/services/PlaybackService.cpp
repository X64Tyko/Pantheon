#include "PlaybackService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../../conf/ConfStore.h"
#include "../../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ctime>

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
			const char* sql = content_type == "movie"
				? "SELECT file_path, duration_ms, title FROM movie WHERE movie_id = ?"
				: "SELECT file_path, duration_ms, title FROM episode WHERE episode_id = ?";
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
	svr.Get("/api/watch-progress", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		int limit = 24;
		if (auto it = req.params.find("limit"); it != req.params.end()) {
			try { limit = std::clamp(std::stoi(it->second), 1, 100); } catch (...) {}
		}

		try {
			SQLite::Statement q(db_.get(), R"SQL(
				SELECT content_type, content_id, position_ms, duration_ms, updated_at
				FROM watch_progress WHERE user_id = ? ORDER BY updated_at DESC LIMIT ?
			)SQL");
			q.bind(1, user->user_id);
			q.bind(2, limit);

			json out = json::array();
			while (q.executeStep()) {
				auto content_type = q.getColumn(0).getString();
				auto content_id   = q.getColumn(1).getString();

				json r;
				r["content_type"] = content_type;
				r["content_id"]   = content_id;
				r["position_ms"]  = q.getColumn(2).getInt64();
				r["duration_ms"]  = q.getColumn(3).getInt64();
				r["updated_at"]   = q.getColumn(4).getInt64();

				if (content_type == "movie") {
					SQLite::Statement m(db_.get(), "SELECT title FROM movie WHERE movie_id = ?");
					m.bind(1, content_id);
					if (!m.executeStep()) continue; // stale reference (deleted item)
					r["title"] = m.getColumn(0).getString();
				} else {
					SQLite::Statement e(db_.get(),
						"SELECT title, season, episode, show_id FROM episode WHERE episode_id = ?");
					e.bind(1, content_id);
					if (!e.executeStep()) continue;
					r["title"]   = e.getColumn(0).getString();
					r["season"]  = e.getColumn(1).getInt();
					r["episode"] = e.getColumn(2).getInt();
					auto show_id = e.getColumn(3).getString();
					r["show_id"] = show_id;

					SQLite::Statement s(db_.get(), "SELECT title FROM show WHERE show_id = ?");
					s.bind(1, show_id);
					r["show_title"] = s.executeStep() ? s.getColumn(0).getString() : "";
				}
				out.push_back(std::move(r));
			}
			route::ok(res, out.dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/watch-progress", e); route::err(res, 500, e.what());
		}
	});

	// ── PUT /api/watch-progress/:content_type/:id ─────────────────────────────
	// Upserts position. An item finished (>=95% through) is treated as watched
	// and its progress row is cleared instead, so it drops off Continue Watching.
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

			if (duration_ms > 0 && position_ms >= static_cast<int64_t>(duration_ms * 0.95)) {
				SQLite::Statement del(db_.get(),
					"DELETE FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");
				del.bind(1, user->user_id); del.bind(2, content_type); del.bind(3, content_id);
				del.exec();
				route::ok(res, json{{"ok", true}, {"watched", true}}.dump());
				return;
			}

			SQLite::Statement ins(db_.get(), R"SQL(
				INSERT INTO watch_progress (user_id, content_type, content_id, position_ms, duration_ms, updated_at)
				VALUES (?, ?, ?, ?, ?, ?)
				ON CONFLICT(user_id, content_type, content_id) DO UPDATE SET
					position_ms = excluded.position_ms,
					duration_ms = excluded.duration_ms,
					updated_at  = excluded.updated_at
			)SQL");
			ins.bind(1, user->user_id);
			ins.bind(2, content_type);
			ins.bind(3, content_id);
			ins.bind(4, position_ms);
			ins.bind(5, duration_ms);
			ins.bind(6, static_cast<int64_t>(std::time(nullptr)));
			ins.exec();

			route::ok(res, json{{"ok", true}, {"watched", false}}.dump());
		} catch (const std::exception& e) {
			route::logErr("PUT /api/watch-progress/:content_type/:id", e); route::err(res, 400, e.what());
		}
	});

	// ── DELETE /api/watch-progress/:content_type/:id ──────────────────────────
	svr.Delete("/api/watch-progress/:content_type/:id", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		auto content_type = req.path_params.at("content_type");
		auto content_id    = req.path_params.at("id");

		try {
			SQLite::Statement del(db_.get(),
				"DELETE FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");
			del.bind(1, user->user_id); del.bind(2, content_type); del.bind(3, content_id);
			del.exec();
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/watch-progress/:content_type/:id", e); route::err(res, 400, e.what());
		}
	});
}
