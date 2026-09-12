#include "WatchTogetherService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../../db/Database.h"
#include "../../db/DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <ctime>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

WatchTogetherService::WatchTogetherService(const ServiceContext& ctx) : db_(ctx.db) {}

namespace {

bool validContentType(const std::string& t) { return t == "movie" || t == "episode"; }

// A session more than this long past created_at with no explicit /close is
// treated as abandoned and excluded from the shelf — Hermes (once its own
// live-presence-based reaping lands) will close these promptly in practice;
// this is only a hygiene backstop for a host who just walks away without
// ever hitting stop, same "self-heals on next read, no backfill needed"
// spirit as SyncManager's own stale-probe gate.
constexpr int64_t kMaxSessionAgeSecs = 24 * 60 * 60;

// Closes any session that's outlived kMaxSessionAgeSecs without being
// explicitly closed — called at the top of every read/write handler below so
// the shelf and join/close endpoints never have to reason about a
// technically-open-but-abandoned row.
void reapStale(Database& db) {
	try {
		SQLite::Statement upd(db.get(),
			"UPDATE watch_together_session SET closed_at = ? WHERE closed_at IS NULL AND created_at < ?");
		auto now = static_cast<int64_t>(std::time(nullptr));
		upd.bind(1, now);
		upd.bind(2, now - kMaxSessionAgeSecs);
		upd.exec();
	} catch (...) {}
}

// Full detail for one session — shared by create/join (a single object) and
// the active-shelf list (one of these per row). nullopt if the session_id
// doesn't exist at all (never true right after create/join, but a real
// possibility for a stale id a client held onto).
std::optional<json> describeSession(Database& db, const std::string& session_id) {
	SQLite::Statement q(db.get(), R"SQL(
		SELECT host_user_id, content_type, content_id, created_at, closed_at
		FROM watch_together_session WHERE session_id = ?
	)SQL");
	q.bind(1, session_id);
	if (!q.executeStep()) return std::nullopt;

	json r;
	r["session_id"]   = session_id;
	auto host_user_id = q.getColumn(0).getString();
	r["host_user_id"] = host_user_id;
	auto content_type = q.getColumn(1).getString();
	auto content_id   = q.getColumn(2).getString();
	r["content_type"] = content_type;
	r["content_id"]   = content_id;
	r["created_at"]   = q.getColumn(3).getInt64();
	r["closed_at"]    = q.getColumn(4).isNull() ? json(nullptr) : json(q.getColumn(4).getInt64());

	r["host_username"] = "";
	try {
		SQLite::Statement u(db.get(), "SELECT username FROM user WHERE user_id = ?");
		u.bind(1, host_user_id);
		if (u.executeStep()) r["host_username"] = u.getColumn(0).getString();
	} catch (...) {}

	// Same movie/episode title-lookup split as PlaybackService's own
	// lookupTitle/Continue-Watching handlers — episode additionally carries
	// season/episode/show_id/show_title so Hades can build the same
	// `/api/shows/{show_id}/thumb` art URL and "SxEy Show Name" label
	// Continue Watching already does, with no new image plumbing needed here.
	r["title"] = "";
	if (content_type == "movie") {
		SQLite::Statement m(db.get(), "SELECT title FROM movie WHERE movie_id = ?");
		m.bind(1, content_id);
		if (m.executeStep()) r["title"] = m.getColumn(0).getString();
	} else {
		SQLite::Statement e(db.get(), "SELECT title, season, episode, show_id FROM episode WHERE episode_id = ?");
		e.bind(1, content_id);
		if (e.executeStep()) {
			r["title"]   = e.getColumn(0).getString();
			r["season"]  = e.getColumn(1).getInt();
			r["episode"] = e.getColumn(2).getInt();
			auto show_id = e.getColumn(3).getString();
			r["show_id"] = show_id;
			SQLite::Statement s(db.get(), "SELECT title FROM show WHERE show_id = ?");
			s.bind(1, show_id);
			r["show_title"] = s.executeStep() ? s.getColumn(0).getString() : "";
		}
	}

	SQLite::Statement mc(db.get(), "SELECT COUNT(*) FROM watch_together_member WHERE session_id = ? AND left_at IS NULL");
	mc.bind(1, session_id);
	r["member_count"] = mc.executeStep() ? mc.getColumn(0).getInt() : 0;

	return r;
}

} // namespace

void WatchTogetherService::registerRoutes(httplib::Server& svr) {

	// ── POST /api/watch-together ────────────────────────────────────────────
	// Starts a new session for the given item, hosted by the caller. The host
	// is also immediately recorded as a member (they're the one about to
	// start watching) — join is otherwise only something OTHER participants
	// call.
	svr.Post("/api/watch-together", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		try {
			auto b            = json::parse(req.body);
			auto content_type = b.value("content_type", "");
			auto content_id   = b.value("content_id", "");
			if (!validContentType(content_type) || content_id.empty()) {
				route::err(res, 400, "content_type must be movie or episode, content_id required"); return;
			}

			reapStale(db_);

			auto session_id = db::generateId();
			auto now        = static_cast<int64_t>(std::time(nullptr));

			SQLite::Statement ins(db_.get(), R"SQL(
				INSERT INTO watch_together_session (session_id, host_user_id, content_type, content_id, created_at)
				VALUES (?, ?, ?, ?, ?)
			)SQL");
			ins.bind(1, session_id); ins.bind(2, user->user_id);
			ins.bind(3, content_type); ins.bind(4, content_id); ins.bind(5, now);
			ins.exec();

			SQLite::Statement mem(db_.get(), R"SQL(
				INSERT INTO watch_together_member (session_id, user_id, joined_at, left_at)
				VALUES (?, ?, ?, NULL)
			)SQL");
			mem.bind(1, session_id); mem.bind(2, user->user_id); mem.bind(3, now);
			mem.exec();

			auto detail = describeSession(db_, session_id);
			route::ok(res, detail->dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/watch-together", e); route::err(res, 400, e.what());
		}
	});

	// ── GET /api/watch-together/active ──────────────────────────────────────
	// The Home shelf feed — every currently-open session, newest first. No
	// per-user filtering beyond "logged in": there's no household/friend
	// concept in the `user` table to key visibility off, and the shelf being
	// visible to every account IS the discovery surface (self-hosted/family
	// server threat model — see the design thread this came out of).
	svr.Get("/api/watch-together/active", [this](const Req&, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		try {
			reapStale(db_);

			std::vector<std::string> ids;
			{
				SQLite::Statement q(db_.get(),
					"SELECT session_id FROM watch_together_session WHERE closed_at IS NULL ORDER BY created_at DESC");
				while (q.executeStep()) ids.push_back(q.getColumn(0).getString());
			}

			json out = json::array();
			for (auto& id : ids) {
				if (auto d = describeSession(db_, id)) out.push_back(std::move(*d));
			}
			route::ok(res, out.dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/watch-together/active", e); route::err(res, 500, e.what());
		}
	});

	// ── GET /api/watch-together/:id ─────────────────────────────────────────
	// Single-session lookup — mainly for Hermes (WatchTogetherManager::
	// getOrCreate), which needs host_user_id/closed_at for a session_id it
	// doesn't have live coordination state for yet, but also generally
	// useful for a client refreshing one session directly rather than
	// re-fetching the whole active list. Any logged-in user, same as the
	// list — a session_id is only reachable if you already know it (shared
	// via the shelf or a direct link), same threat model as the shelf itself.
	svr.Get("/api/watch-together/:id", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		try {
			auto id = req.path_params.at("id");
			auto detail = describeSession(db_, id);
			if (!detail) { route::err(res, 404, "Not found"); return; }
			route::ok(res, detail->dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/watch-together/:id", e); route::err(res, 400, e.what());
		}
	});

	// ── POST /api/watch-together/:id/join ───────────────────────────────────
	// Upsert, not insert — a rejoin (browser refresh, app backgrounded and
	// reopened) just clears left_at rather than erroring on the existing
	// PRIMARY KEY (session_id, user_id) row. Returns the same full detail
	// shape as create, so a joining client can open the player without a
	// second round trip.
	svr.Post("/api/watch-together/:id/join", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		try {
			reapStale(db_);
			auto id = req.path_params.at("id");

			SQLite::Statement chk(db_.get(), "SELECT closed_at FROM watch_together_session WHERE session_id = ?");
			chk.bind(1, id);
			if (!chk.executeStep()) { route::err(res, 404, "Not found"); return; }
			if (!chk.getColumn(0).isNull()) { route::err(res, 410, "Session has ended"); return; }

			auto now = static_cast<int64_t>(std::time(nullptr));
			SQLite::Statement ups(db_.get(), R"SQL(
				INSERT INTO watch_together_member (session_id, user_id, joined_at, left_at)
				VALUES (?, ?, ?, NULL)
				ON CONFLICT (session_id, user_id) DO UPDATE SET joined_at = excluded.joined_at, left_at = NULL
			)SQL");
			ups.bind(1, id); ups.bind(2, user->user_id); ups.bind(3, now);
			ups.exec();

			auto detail = describeSession(db_, id);
			route::ok(res, detail->dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/watch-together/:id/join", e); route::err(res, 400, e.what());
		}
	});

	// ── POST /api/watch-together/:id/leave ──────────────────────────────────
	// Any current member, including the host, may leave — this only affects
	// this one participant's own membership row (member_count drops by one);
	// it does NOT end the session for everyone else. Ending it for everyone
	// is the host-only /close below.
	svr.Post("/api/watch-together/:id/leave", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		try {
			auto id  = req.path_params.at("id");
			auto now = static_cast<int64_t>(std::time(nullptr));
			SQLite::Statement upd(db_.get(),
				"UPDATE watch_together_member SET left_at = ? WHERE session_id = ? AND user_id = ? AND left_at IS NULL");
			upd.bind(1, now); upd.bind(2, id); upd.bind(3, user->user_id);
			upd.exec();
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/watch-together/:id/leave", e); route::err(res, 400, e.what());
		}
	});

	// ── POST /api/watch-together/:id/close ──────────────────────────────────
	// Host-only (or admin, for moderation) — ends the session for everyone.
	// This is Kairos's own explicit-close path; Hermes calls the same route
	// once its live-presence reaping decides the host is genuinely gone
	// (Phase D), rather than duplicating the closed_at bookkeeping itself.
	svr.Post("/api/watch-together/:id/close", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		try {
			auto id = req.path_params.at("id");

			SQLite::Statement chk(db_.get(), "SELECT host_user_id, closed_at FROM watch_together_session WHERE session_id = ?");
			chk.bind(1, id);
			if (!chk.executeStep()) { route::err(res, 404, "Not found"); return; }
			auto host_user_id = chk.getColumn(0).getString();
			if (user->user_id != host_user_id && user->role != "admin") { route::err(res, 403, "Forbidden"); return; }
			if (!chk.getColumn(1).isNull()) { route::ok(res, json{{"ok", true}}.dump()); return; } // already closed

			SQLite::Statement upd(db_.get(), "UPDATE watch_together_session SET closed_at = ? WHERE session_id = ?");
			upd.bind(1, static_cast<int64_t>(std::time(nullptr))); upd.bind(2, id);
			upd.exec();
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/watch-together/:id/close", e); route::err(res, 400, e.what());
		}
	});
}
