#include "AuthService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../../auth/AuthStore.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

AuthService::AuthService(const ServiceContext& ctx) : auth_(ctx.auth) {}

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
		route::ok(res, json{{"token", token}, {"user", {{"user_id", user->user_id}, {"username", user->username}, {"role", user->role}}}}.dump());
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
		route::ok(res, json{{"token", token}, {"user", {{"user_id", user->user_id}, {"username", user->username}, {"role", user->role}}}}.dump());
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
		const auto& u = *currentUser();
		route::ok(res, json{{"user_id", u.user_id}, {"username", u.username}, {"role", u.role}}.dump());
	});

	svr.Get("/api/users", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json arr = json::array();
		for (const auto& u : auth_.listUsers())
			arr.push_back({{"user_id", u.user_id}, {"username", u.username}, {"role", u.role}});
		route::ok(res, arr.dump());
	});

	svr.Post("/api/users", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json body;
		try { body = json::parse(req.body); } catch (...) { route::err(res, 400, "Invalid JSON"); return; }
		const std::string username = body.value("username", "");
		const std::string password = body.value("password", "");
		const std::string role     = body.value("role", "viewer");
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
}
