#include "RequestService.h"
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

RequestService::RequestService(const ServiceContext& ctx) : db_(ctx.db) {}

void RequestService::registerRoutes(httplib::Server& svr) {

	// ── GET /api/requests ─────────────────────────────────────────────────────
	// Admin: all requests ordered by created_at desc.
	// Viewer: only their own requests.
	svr.Get("/api/requests", [this](const Req&, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		try {
			const bool isAdmin = (user->role == "admin");
			const std::string sql = isAdmin
				? "SELECT request_id, user_id, content_type, source, external_id, title, year, poster_url, status, created_at FROM content_request ORDER BY created_at DESC"
				: "SELECT request_id, user_id, content_type, source, external_id, title, year, poster_url, status, created_at FROM content_request WHERE user_id = ? ORDER BY created_at DESC";

			SQLite::Statement q(db_.get(), sql);
			if (!isAdmin) q.bind(1, user->user_id);

			json out = json::array();
			while (q.executeStep()) {
				json r;
				r["request_id"]   = q.getColumn(0).getString();
				r["user_id"]      = q.getColumn(1).getString();
				r["content_type"] = q.getColumn(2).getString();
				r["source"]       = q.getColumn(3).getString();
				r["external_id"]  = q.getColumn(4).getString();
				r["title"]        = q.getColumn(5).getString();
				r["year"]         = q.getColumn(6).isNull() ? json(nullptr) : json(q.getColumn(6).getInt());
				r["poster_url"]   = q.getColumn(7).getString();
				r["status"]       = q.getColumn(8).getString();
				r["created_at"]   = q.getColumn(9).getInt64();
				out.push_back(std::move(r));
			}
			route::ok(res, out.dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/requests", e); route::err(res, 500, e.what());
		}
	});

	// ── POST /api/requests ────────────────────────────────────────────────────
	// Any authenticated user may create a request.
	svr.Post("/api/requests", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		try {
			auto b            = json::parse(req.body);
			auto content_type = b.value("content_type", "");
			auto source       = b.value("source", "");
			auto external_id  = b.value("external_id", "");
			auto title        = b.value("title", "");
			if (content_type.empty() || source.empty() || external_id.empty() || title.empty()) {
				route::err(res, 400, "content_type, source, external_id, title required"); return;
			}

			auto request_id = db::generateId();
			auto now        = static_cast<int64_t>(std::time(nullptr));

			SQLite::Statement ins(db_.get(), R"SQL(
				INSERT OR IGNORE INTO content_request
					(request_id, user_id, content_type, source, external_id, title, year, poster_url, status, created_at)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'pending', ?)
			)SQL");
			ins.bind(1, request_id);
			ins.bind(2, user->user_id);
			ins.bind(3, content_type);
			ins.bind(4, source);
			ins.bind(5, external_id);
			ins.bind(6, title);
			if (b.contains("year") && !b["year"].is_null()) ins.bind(7, b["year"].get<int>());
			else ins.bind(7); // NULL
			ins.bind(8, b.value("poster_url", ""));
			ins.bind(9, now);
			ins.exec();

			if (db_.get().getChanges() == 0) {
				// Already exists — fetch and return the existing one
				SQLite::Statement sel(db_.get(), "SELECT request_id, status FROM content_request WHERE user_id=? AND content_type=? AND source=? AND external_id=?");
				sel.bind(1, user->user_id); sel.bind(2, content_type); sel.bind(3, source); sel.bind(4, external_id);
				if (sel.executeStep()) {
					route::ok(res, json{{"request_id", sel.getColumn(0).getString()}, {"status", sel.getColumn(1).getString()}, {"duplicate", true}}.dump());
					return;
				}
			}
			route::ok(res, json{{"request_id", request_id}, {"status", "pending"}}.dump());
		} catch (const std::exception& e) {
			route::logErr("POST /api/requests", e); route::err(res, 400, e.what());
		}
	});

	// ── PATCH /api/requests/:id ───────────────────────────────────────────────
	// Admin only: approve or reject.
	svr.Patch("/api/requests/:id", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user || user->role != "admin") { route::err(res, 403, "Forbidden"); return; }

		try {
			auto b      = json::parse(req.body);
			auto status = b.value("status", "");
			if (status != "approved" && status != "rejected") {
				route::err(res, 400, "status must be 'approved' or 'rejected'"); return;
			}
			auto id = req.path_params.at("id");
			SQLite::Statement upd(db_.get(), "UPDATE content_request SET status=? WHERE request_id=?");
			upd.bind(1, status); upd.bind(2, id);
			upd.exec();
			if (db_.get().getChanges() == 0) { route::err(res, 404, "Not found"); return; }
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/requests/:id", e); route::err(res, 400, e.what());
		}
	});

	// ── DELETE /api/requests/:id ──────────────────────────────────────────────
	// Owner or admin may delete.
	svr.Delete("/api/requests/:id", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		try {
			auto id = req.path_params.at("id");
			if (user->role == "admin") {
				SQLite::Statement del(db_.get(), "DELETE FROM content_request WHERE request_id=?");
				del.bind(1, id); del.exec();
			} else {
				SQLite::Statement del(db_.get(), "DELETE FROM content_request WHERE request_id=? AND user_id=?");
				del.bind(1, id); del.bind(2, user->user_id); del.exec();
			}
			if (db_.get().getChanges() == 0) { route::err(res, 404, "Not found"); return; }
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/requests/:id", e); route::err(res, 400, e.what());
		}
	});
}
