#include "AuthService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../../auth/AuthStore.h"
#include "../../db/ConfigRepository.h"
#include "../../db/RestrictionRepository.h"
#include "../../email/EmailService.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

AuthService::AuthService(const ServiceContext& ctx)
	: auth_(ctx.auth), db_(ctx.db), email_(ctx.email) {}

namespace {
json userJson(const AuthUser& u) {
	return {
		{"user_id",              u.user_id},
		{"username",             u.username},
		{"role",                 u.role},
		{"restricted",           u.restricted},
		{"max_tv_rating",        u.max_tv_rating},
		{"max_movie_rating",     u.max_movie_rating},
		{"max_channel_rating",   u.max_channel_rating},
		{"must_change_password", u.must_change_password},
	};
}
}

void AuthService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/auth/setup", [this](const Req&, Res& res) {
		route::ok(res, json{{"setup_required", !auth_.hasAnyUser()}}.dump());
	});

	svr.Post("/api/auth/setup", [this](const Req& req, Res& res) {
		if (auth_.hasAnyUser()) { route::err(res, 409, "Setup already complete"); return; }
		json body;
		try { body = json::parse(req.body); } catch (...) { route::err(res, 400, "Invalid JSON"); return; }
		const std::string username = body.value("username", "");
		const std::string password = body.value("password", "");
		if (username.empty() || password.empty()) { route::err(res, 400, "username and password required"); return; }
		if (!auth_.createUser(username, password, "admin")) { route::err(res, 500, "Failed to create user"); return; }
		const std::string token = auth_.login(username, password);
		auto user = auth_.validate(token);
		route::ok(res, json{{"token", token}, {"user", userJson(*user)}}.dump());
	});

	svr.Post("/api/auth/login", [this](const Req& req, Res& res) {
		json body;
		try { body = json::parse(req.body); } catch (...) { route::err(res, 400, "Invalid JSON"); return; }
		const std::string username = body.value("username", "");
		const std::string password = body.value("password", "");
		if (username.empty() || password.empty()) { route::err(res, 400, "username and password required"); return; }
		const std::string token = auth_.login(username, password);
		if (token.empty()) { route::err(res, 401, "Invalid credentials"); return; }
		auto user = auth_.validate(token);
		route::ok(res, json{{"token", token}, {"user", userJson(*user)}}.dump());
	});

	svr.Post("/api/auth/logout", [this](const Req& req, Res& res) {
		if (req.has_header("Authorization")) {
			const std::string& hdr = req.get_header_value("Authorization");
			if (hdr.starts_with("Bearer ")) auth_.logout(hdr.substr(7));
		}
		route::ok(res, json{{"ok", true}}.dump());
	});

	svr.Get("/api/auth/me", [](const Req&, Res& res) {
		if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
		route::ok(res, userJson(*currentUser()).dump());
	});

	// Invite-claim flow — unauthenticated (see Router::isPublicPath), reached
	// before the person has any session at all. Only actionable with the
	// unguessable token in the path itself; see AuthStore::claimInvite.
	svr.Get("/api/auth/invite/:token", [this](const Req& req, Res& res) {
		auto username = auth_.getInviteUsername(req.path_params.at("token"));
		if (!username) { route::err(res, 404, "Invalid or expired invite"); return; }
		route::ok(res, json{{"username", *username}}.dump());
	});

	svr.Post("/api/auth/invite/:token", [this](const Req& req, Res& res) {
		json body;
		try { body = json::parse(req.body); } catch (...) { route::err(res, 400, "Invalid JSON"); return; }
		const std::string password = body.value("password", "");
		if (password.empty()) { route::err(res, 400, "password required"); return; }
		const std::string token = auth_.claimInvite(req.path_params.at("token"), password);
		if (token.empty()) { route::err(res, 404, "Invalid or expired invite"); return; }
		auto user = auth_.validate(token);
		route::ok(res, json{{"token", token}, {"user", userJson(*user)}}.dump());
	});

	// Re-issues a fresh invite link for an account that never claimed its
	// first one (e.g. the email never arrived) — the old token is left to
	// expire naturally rather than explicitly invalidated.
	svr.Post("/api/users/:id/resend-invite", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		const std::string invite_token = auth_.resendInvite(req.path_params.at("id"));
		if (invite_token.empty()) { route::err(res, 400, "No pending invite for this user"); return; }

		std::string base_url = ConfigRepository(db_).getValue("smtp_public_base_url");
		while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
		route::ok(res, json{{"ok", true}, {"invite_link", base_url + "/invite/" + invite_token}}.dump());
	});

	svr.Get("/api/users", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json arr = json::array();
		for (const auto& u : auth_.listUsers())
			arr.push_back(userJson(u));
		route::ok(res, arr.dump());
	});

	// invite.method: 'password' (default — client supplies `password` directly,
	// identical to this route's original behavior), 'temp_password' (server
	// generates one, returned once in the response for the admin to relay),
	// or 'email' (no password set yet; a claim link is emailed to invite.email
	// if SMTP is configured — the link is also always returned so the admin
	// can hand it out manually if sending fails or SMTP isn't set up).
	svr.Post("/api/users", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json body;
		try { body = json::parse(req.body); } catch (...) { route::err(res, 400, "Invalid JSON"); return; }
		const std::string username = body.value("username", "");
		const std::string role     = body.value("role", "viewer");
		const std::string method   = body.contains("invite") ? body["invite"].value("method", "password") : "password";

		if (method == "temp_password") {
			auto [user_id, temp_password] = auth_.createUserWithTempPassword(username, role);
			if (user_id.empty()) { route::err(res, 409, "Username taken or invalid input"); return; }
			route::ok(res, json{{"ok", true}, {"user_id", user_id}, {"temp_password", temp_password}}.dump());
			return;
		}

		if (method == "email") {
			const std::string invite_email = body["invite"].value("email", "");
			if (invite_email.empty()) { route::err(res, 400, "invite.email required for method 'email'"); return; }
			auto [user_id, invite_token] = auth_.createUserWithEmailInvite(username, role);
			if (user_id.empty()) { route::err(res, 409, "Username taken or invalid input"); return; }

			std::string base_url = ConfigRepository(db_).getValue("smtp_public_base_url");
			while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
			const std::string invite_link = base_url + "/invite/" + invite_token;

			std::string send_error;
			const bool sent = email_.sendInviteEmail(invite_email, username, invite_link, &send_error);
			json out{{"ok", true}, {"user_id", user_id}, {"invite_link", invite_link}, {"invite_sent", sent}};
			if (!sent) out["invite_error"] = send_error;
			route::ok(res, out.dump());
			return;
		}

		const std::string password = body.value("password", "");
		if (!auth_.createUser(username, password, role)) { route::err(res, 409, "Username taken or invalid input"); return; }
		route::ok(res, json{{"ok", true}}.dump());
	});

	svr.Patch("/api/users/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json body;
		try { body = json::parse(req.body); } catch (...) { route::err(res, 400, "Invalid JSON"); return; }
		const std::string password = body.value("password", "");
		const std::string role     = body.value("role", "");
		if (!auth_.updateUser(req.path_params.at("id"), password, role)) { route::err(res, 400, "Invalid role"); return; }
		route::ok(res, json{{"ok", true}}.dump());
	});

	svr.Delete("/api/users/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		if (!auth_.deleteUser(req.path_params.at("id"), currentUser()->user_id)) {
			route::err(res, 409, "Cannot delete self or last admin"); return;
		}
		route::ok(res, json{{"ok", true}}.dump());
	});

	// Parental-controls settings — separate from the credentials/role PATCH
	// above so the Users page can save one concern without resending the other.
	svr.Patch("/api/users/:id/restriction", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json body;
		try { body = json::parse(req.body); } catch (...) { route::err(res, 400, "Invalid JSON"); return; }
		auth_.updateRestriction(
			req.path_params.at("id"),
			body.value("restricted", false),
			body.value("max_tv_rating", "TV-Y"),
			body.value("max_movie_rating", "G"),
			body.value("max_channel_rating", "TV-Y"));
		route::ok(res, json{{"ok", true}}.dump());
	});

	svr.Get("/api/users/:id/overrides", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json arr = json::array();
		for (const auto& o : RestrictionRepository(db_).listOverrides(req.path_params.at("id")))
			arr.push_back({{"entity_type", o.entity_type}, {"entity_id", o.entity_id}, {"mode", o.mode}, {"title", o.title}});
		route::ok(res, arr.dump());
	});

	svr.Post("/api/users/:id/overrides", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json body;
		try { body = json::parse(req.body); } catch (...) { route::err(res, 400, "Invalid JSON"); return; }
		const std::string entity_type = body.value("entity_type", "");
		const std::string entity_id   = body.value("entity_id", "");
		const std::string mode        = body.value("mode", "");
		if ((entity_type != "show" && entity_type != "movie" && entity_type != "channel")
		    || entity_id.empty() || (mode != "allow" && mode != "block")) {
			route::err(res, 400, "entity_type, entity_id, and mode ('allow'|'block') required"); return;
		}
		RestrictionRepository(db_).setOverride(req.path_params.at("id"), entity_type, entity_id, mode);
		route::ok(res, json{{"ok", true}}.dump());
	});

	svr.Delete("/api/users/:id/overrides/:entity_type/:entity_id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		RestrictionRepository(db_).removeOverride(
			req.path_params.at("id"), req.path_params.at("entity_type"), req.path_params.at("entity_id"));
		route::ok(res, json{{"ok", true}}.dump());
	});

	// Mints a viewer-capped session for handing off to a Cast receiver (see
	// hades/src/cast/ and pantheon-relay) — any authenticated user may mint
	// one for themselves. The token is returned exactly once here and never
	// re-exposed; session_id is what listSessions()/revokeSession() below
	// operate on instead.
	svr.Post("/api/auth/cast-token", [this](const Req&, Res& res) {
		if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
		auto [token, session_id] = auth_.mintCastToken(currentUser()->user_id);
		route::ok(res, json{{"token", token}, {"session_id", session_id}}.dump());
	});

	// A user's own Cast devices — not admin-gated, this is "my devices," not
	// a cross-user panel. purpose is currently always 'cast' in practice
	// (the only other purpose, 'login', isn't surfaced through this path).
	svr.Get("/api/auth/sessions", [this](const Req& req, Res& res) {
		if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
		std::string purpose = req.has_param("purpose") ? req.get_param_value("purpose") : "cast";
		json arr = json::array();
		for (const auto& s : auth_.listSessions(currentUser()->user_id, purpose))
			arr.push_back({{"session_id", s.session_id}, {"created_at", s.created_at}, {"last_seen", s.last_seen}});
		route::ok(res, arr.dump());
	});

	svr.Delete("/api/auth/sessions/:id", [this](const Req& req, Res& res) {
		if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
		if (!auth_.revokeSession(currentUser()->user_id, req.path_params.at("id"))) {
			route::err(res, 404, "Session not found"); return;
		}
		route::ok(res, json{{"ok", true}}.dump());
	});
}
