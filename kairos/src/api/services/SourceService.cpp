#include "SourceService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../../auth/AuthStore.h"
#include "../../db/ConfigRepository.h"
#include "../../db/ContentRepository.h"
#include "../../db/SourceRepository.h"
#include "../../email/EmailService.h"
#include "log/LogBuffer.h"
#include "../../source/SyncManager.h"
#include "../../source/IMediaSource.h"
#include "../../source/LocalSource.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <httplib.h>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

SourceService::SourceService(const ServiceContext& ctx)
	: db_(ctx.db), sync_(ctx.sync), logs_(ctx.logs), auth_(ctx.auth), email_(ctx.email) {}

void SourceService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/sources/types", [](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json types = {
			{{"type","plex"},     {"display_name","Plex"},        {"supported",true}},
			{{"type","jellyfin"}, {"display_name","Jellyfin"},    {"supported",true}},
			{{"type","emby"},     {"display_name","Emby"},        {"supported",true}},
			{{"type","local"},    {"display_name","Local Media"}, {"supported",true}},
		};
		route::ok(res, types.dump());
	});

	svr.Get("/api/sources", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json result = json::array();
		for (const auto& s : SourceRepository(db_).listSources())
			result.push_back({{"source_id", s.source_id}, {"source_type", s.source_type},
			                  {"display_name", s.display_name}, {"base_url", s.base_url},
			                  {"enabled", s.enabled}, {"synced_user_id", s.synced_user_id},
			                  {"user_sync_error", s.user_sync_error}, {"sync_priority", s.sync_priority},
			                  {"auto_writeback", s.auto_writeback},
			                  {"writeback_update_art", s.writeback_update_art},
			                  {"writeback_update_external_ids", s.writeback_update_external_ids},
			                  {"writeback_update_collections", s.writeback_update_collections}});
		route::ok(res, result.dump());
	});

	svr.Post("/api/sources", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			auto b = json::parse(req.body);
			std::string source_id    = b.value("source_id",    "");
			std::string source_type  = b.value("source_type",  "");
			std::string display_name = b.value("display_name", "");
			std::string base_url     = b.value("base_url",     "");
			int sync_priority        = b.value("sync_priority", 0);
			if (source_id.empty() || source_type.empty() || display_name.empty()) {
				route::err(res, 400, "source_id, source_type, and display_name required");
				return;
			}
			SourceRepository(db_).createSource(source_id, source_type, display_name, base_url, sync_priority);
			sync_.loadSources();
			res.status = 201;
			route::ok(res, json{{"source_id", source_id}}.dump());
		} catch (const SQLite::Exception& e) {
			route::logErr("POST /api/sources", e); route::err(res, 409, e.what());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("POST /api/sources", e); route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/sources/test", [](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			auto b = json::parse(req.body);
			std::string source_type = b.value("source_type", "");
			std::string base_url    = b.value("base_url",    "");
			std::string token       = b.value("token",       "");
			std::string user_id     = b.value("user_id",     "");

			if (source_type == "plex") {
				if (base_url.empty() || token.empty()) {
					route::err(res, 400, "base_url and token are required"); return;
				}
				httplib::Client client(base_url);
				client.set_default_headers({{"X-Plex-Token", token}, {"Accept", "application/json"}});
				client.set_connection_timeout(10);
				client.set_read_timeout(10);
				auto r = client.Get("/library/sections");
				if (!r) {
					route::ok(res, json{{"ok", false},
					    {"error", "Cannot connect to " + base_url + ": " +
					              httplib::to_string(r.error())}}.dump());
					return;
				}
				if (r->status == 401 || r->status == 403) {
					route::ok(res, json{{"ok", false},
					    {"error", "Authentication failed — check your Plex token"}}.dump());
					return;
				}
				if (r->status != 200) {
					route::ok(res, json{{"ok", false},
					    {"error", "Unexpected response: HTTP " + std::to_string(r->status)}}.dump());
					return;
				}
				route::ok(res, json{{"ok", true}}.dump());
				return;
			}

			if (source_type == "jellyfin" || source_type == "emby") {
				if (base_url.empty() || token.empty()) {
					route::err(res, 400, "base_url and token are required"); return;
				}
				const std::string auth_header =
				    (source_type == "jellyfin") ? "X-MediaBrowser-Token" : "X-Emby-Token";
				httplib::Client client(base_url);
				client.set_default_headers({{auth_header, token}, {"Accept", "application/json"}});
				client.set_connection_timeout(10);
				client.set_read_timeout(10);
				auto r = client.Get("/System/Info");
				if (!r) {
					route::ok(res, json{{"ok", false},
					    {"error", "Cannot connect to " + base_url + ": " +
					              httplib::to_string(r.error())}}.dump());
					return;
				}
				if (r->status == 401 || r->status == 403) {
					route::ok(res, json{{"ok", false},
					    {"error", "Authentication failed — check your API token"}}.dump());
					return;
				}
				if (r->status != 200) {
					route::ok(res, json{{"ok", false},
					    {"error", "Unexpected response: HTTP " + std::to_string(r->status)}}.dump());
					return;
				}
				route::ok(res, json{{"ok", true}}.dump());
				return;
			}

			if (source_type == "local") {
				if (base_url.empty()) {
					route::err(res, 400, "base_url (directory path) is required"); return;
				}
				std::error_code ec;
				const bool exists = std::filesystem::is_directory(base_url, ec);
				if (ec || !exists) {
					route::ok(res, json{{"ok", false},
					    {"error", "Directory not found or not accessible: " + base_url}}.dump());
					return;
				}
				route::ok(res, json{{"ok", true}}.dump());
				return;
			}

			route::err(res, 400, "unknown source_type: " + source_type);
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::ok(res, json{{"ok", false}, {"error", std::string(e.what())}}.dump());
		}
	});

	svr.Delete("/api/sources/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			SourceRepository(db_).removeSource(id);
			sync_.loadSources();
			logs_.push("[api] deleted source: " + id);
			route::ok(res, json{{"deleted", id}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/sources/" + id, e);
			route::err(res, 500, e.what());
		}
	});

	// Source-level settings not tied to a specific library: synced_user_id
	// (watch-state sync target — see WatchProgressRepository /
	// SyncManager::syncMovies), sync_priority, and the writeback settings
	// (auto_writeback + the per-field update_* opt-outs — see
	// MediaSourceConfig's own comments and ContentService::writebackShow/
	// writebackMovie for how they're consumed).
	svr.Patch("/api/sources/:id", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		try {
			auto b = json::parse(req.body);
			SourceRepository repo(db_);
			if (!repo.getSource(id)) { route::err(res, 404, "source not found"); return; }
			if (b.contains("synced_user_id")) {
				std::string user_id = b["synced_user_id"].is_null() ? "" : b["synced_user_id"].get<std::string>();
				repo.setSyncedUserId(id, user_id);
			}
			if (b.contains("sync_priority")) {
				repo.setSyncPriority(id, b["sync_priority"].get<int>());
				sync_.loadSources();
			}
			if (b.contains("auto_writeback"))
				repo.setAutoWriteback(id, b["auto_writeback"].get<bool>());
			if (b.contains("writeback_update_art"))
				repo.setWritebackUpdateArt(id, b["writeback_update_art"].get<bool>());
			if (b.contains("writeback_update_external_ids"))
				repo.setWritebackUpdateExternalIds(id, b["writeback_update_external_ids"].get<bool>());
			if (b.contains("writeback_update_collections"))
				repo.setWritebackUpdateCollections(id, b["writeback_update_collections"].get<bool>());
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/sources/:id", e);
			route::err(res, 500, e.what());
		}
	});

	// Every source-reported account with no local Pantheon account imported
	// for it yet — see SourceRepository::listUnmappedSourceUsers().
	svr.Get("/api/sources/unmapped-users", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			json result = json::array();
			for (const auto& u : SourceRepository(db_).listUnmappedSourceUsers())
				result.push_back({{"source_id", u.source_id},
				                  {"source_display_name", u.source_display_name},
				                  {"external_user_id", u.external_user_id},
				                  {"display_name", u.display_name},
				                  {"email", u.email}});
			route::ok(res, result.dump());
		} catch (const std::exception& e) {
			route::logErr("GET /api/sources/unmapped-users", e);
			route::err(res, 500, e.what());
		}
	});

	// Creates a local Pantheon account for a discovered source user (see
	// Feature 3/4 — invite-based account creation). Picks the invite method
	// automatically: email (if the source gave one AND SMTP is configured),
	// else a server-generated temp password the admin relays manually.
	svr.Post("/api/sources/:id/users/:external_id/import", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto source_id        = req.path_params.at("id");
		auto external_user_id = req.path_params.at("external_id");
		try {
			json b = req.body.empty() ? json::object() : json::parse(req.body);
			std::string role = b.value("role", "viewer");
			if (role != "admin" && role != "viewer") { route::err(res, 400, "role must be admin or viewer"); return; }

			SourceRepository repo(db_);
			auto su = repo.getSourceUser(source_id, external_user_id);
			if (!su) { route::err(res, 404, "source user not found"); return; }
			// Without this, a double-click or a retried request after a network
			// hiccup silently creates a second local account for the same
			// source user — the route otherwise has no way to tell "already
			// imported" from "never imported".
			if (!su->imported_user_id.empty()) { route::err(res, 409, "already imported"); return; }

			// Dedupe the source's display name against existing usernames
			// (unlike the single-account create form, a bulk import shouldn't
			// hard-fail on a name collision) — "Chris", "Chris2", "Chris3", ...
			const std::string base_username = su->display_name.empty() ? external_user_id : su->display_name;
			std::string username = base_username;
			{
				auto existing = auth_.listUsers();
				auto taken = [&](const std::string& u) {
					return std::any_of(existing.begin(), existing.end(),
						[&](const AuthUser& e) { return e.username == u; });
				};
				for (int suffix = 2; taken(username); ++suffix)
					username = base_username + std::to_string(suffix);
			}

			ConfigRepository cfg_repo(db_);
			const bool smtp_configured = !cfg_repo.getValue("smtp_host").empty()
			                           && !cfg_repo.getValue("smtp_from_address").empty();

			json out{{"ok", true}, {"username", username}};
			std::string user_id;
			if (!su->email.empty() && smtp_configured) {
				auto [uid, invite_token] = auth_.createUserWithEmailInvite(username, role);
				if (uid.empty()) { route::err(res, 409, "failed to create account"); return; }
				user_id = uid;

				std::string base_url = cfg_repo.getValue("smtp_public_base_url");
				while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
				const std::string invite_link = base_url + "/invite/" + invite_token;

				std::string send_error;
				const bool sent = email_.sendInviteEmail(su->email, username, invite_link, &send_error);
				out["invite_method"] = "email";
				out["invite_link"]   = invite_link;
				out["invite_sent"]   = sent;
				if (!sent) out["invite_error"] = send_error;
			} else {
				auto [uid, temp_password] = auth_.createUserWithTempPassword(username, role);
				if (uid.empty()) { route::err(res, 409, "failed to create account"); return; }
				user_id = uid;
				out["invite_method"] = "temp_password";
				out["temp_password"] = temp_password;
			}

			out["user_id"] = user_id;
			repo.setImportedUserId(source_id, external_user_id, user_id);
			route::ok(res, out.dump());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("POST /api/sources/:id/users/:external_id/import", e);
			route::err(res, 500, e.what());
		}
	});

	// Every discovered account for one source, mapped or not — powers the
	// "Per-Account Watch Sync" panel (unlike GET /api/sources/unmapped-users,
	// which only ever lists unlinked ones for the global nag popup).
	svr.Get("/api/sources/:id/users", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto source_id = req.path_params.at("id");
		json result = json::array();
		for (const auto& u : SourceRepository(db_).listSourceUsers(source_id))
			result.push_back({{"external_user_id", u.external_user_id},
			                  {"display_name", u.display_name},
			                  {"email", u.email},
			                  {"imported_user_id", u.imported_user_id}});
		route::ok(res, result.dump());
	});

	// Links a discovered source account to an *existing* local user, so its
	// own watch history gets pulled during sync (see
	// SyncManager::syncLinkedUserWatchState) — the "already have an account"
	// counterpart to POST .../import above, which only ever creates a new one.
	svr.Put("/api/sources/:id/users/:external_id/link", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto source_id        = req.path_params.at("id");
		auto external_user_id = req.path_params.at("external_id");
		try {
			auto b = json::parse(req.body);
			std::string user_id = b.value("user_id", "");
			if (user_id.empty()) { route::err(res, 400, "user_id is required"); return; }

			SourceRepository repo(db_);
			auto su = repo.getSourceUser(source_id, external_user_id);
			if (!su) { route::err(res, 404, "source user not found"); return; }
			if (!su->imported_user_id.empty()) { route::err(res, 409, "already linked"); return; }

			auto existing = auth_.listUsers();
			bool user_exists = std::any_of(existing.begin(), existing.end(),
				[&](const AuthUser& u) { return u.user_id == user_id; });
			if (!user_exists) { route::err(res, 404, "user not found"); return; }

			repo.setImportedUserId(source_id, external_user_id, user_id);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("PUT /api/sources/:id/users/:external_id/link", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Delete("/api/sources/:id/users/:external_id/link", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto source_id        = req.path_params.at("id");
		auto external_user_id = req.path_params.at("external_id");
		SourceRepository(db_).unlinkSourceUser(source_id, external_user_id);
		route::ok(res, json{{"ok", true}}.dump());
	});

	svr.Get("/api/sources/:id/libraries/available", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id  = req.path_params.at("id");
		auto src = sync_.findSource(id);
		if (!src)                { route::err(res, 404, "source not found or not loaded"); return; }
		if (!src->isSupported()) { route::err(res, 501, src->sourceType() + " not yet supported"); return; }
		auto libs = src->listAvailableLibraries();
		json result = json::array();
		for (const auto& lib : libs)
			result.push_back({{"external_lib_id", lib.external_lib_id},
			                  {"name", lib.name}, {"type", lib.type}});
		route::ok(res, result.dump());
	});

	svr.Get("/api/sources/:id/fs", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id  = req.path_params.at("id");
		auto src = sync_.findSource(id);
		if (!src)                         { route::err(res, 404, "source not found"); return; }
		auto* local = dynamic_cast<LocalSource*>(src);
		if (!local)                       { route::err(res, 400, "not a local source"); return; }
		std::string path = req.has_param("path") ? req.get_param_value("path") : "";
		auto entries = local->listSubdirectories(path);
		json result = json::array();
		for (const auto& e : entries)
			result.push_back({{"external_lib_id", e.external_lib_id}, {"name", e.name}, {"type", e.type}});
		route::ok(res, result.dump());
	});

	svr.Get("/api/sources/:id/libraries", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto source_id = req.path_params.at("id");
		json result = json::array();
		for (const auto& lib : SourceRepository(db_).listLibraries(source_id))
			result.push_back({{"library_id", lib.library_id}, {"source_id", lib.source_id},
			                  {"external_lib_id", lib.external_lib_id},
			                  {"display_name", lib.display_name}, {"library_type", lib.library_type},
			                  {"preferred_scraper", lib.preferred_scraper},
			                  {"preferred_language", lib.preferred_language},
			                  {"include_anidb", lib.include_anidb},
			                  {"enabled", lib.enabled},
			                  {"show_on_home", lib.show_on_home},
			                  {"skip_scraping", lib.skip_scraping}});
		route::ok(res, result.dump());
	});

	svr.Post("/api/sources/:id/libraries", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		try {
			auto source_id = req.path_params.at("id");
			auto b = json::parse(req.body);
			std::string external_lib_id    = b.value("external_lib_id",    "");
			std::string display_name       = b.value("display_name",       "");
			std::string library_type       = b.value("library_type",       "show");
			std::string preferred_scraper  = b.value("preferred_scraper",  "");
			std::string preferred_language = b.value("preferred_language", "");
			bool        include_anidb      = b.value("include_anidb",      false);
			if (external_lib_id.empty() || display_name.empty()) {
				route::err(res, 400, "external_lib_id and display_name required"); return;
			}
			if (library_type != "show" && library_type != "movie" && library_type != "mixed") {
				route::err(res, 400, "library_type must be one of: show, movie, mixed"); return;
			}
			std::string library_id = SourceRepository(db_).createLibrary(
				source_id, external_lib_id, display_name, library_type,
				preferred_scraper, preferred_language, include_anidb);
			res.status = 201;
			route::ok(res, json{{"library_id", library_id}}.dump());
		} catch (const SQLite::Exception& e) {
			route::logErr("POST /api/sources/:id/libraries", e); route::err(res, 409, e.what());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("POST /api/sources/:id/libraries", e); route::err(res, 500, e.what());
		}
	});

	svr.Patch("/api/sources/:id/libraries/:lid", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto lid = req.path_params.at("lid");
		try {
			auto b = json::parse(req.body);
			SourceRepository repo(db_);
			// Fetch current values so we can apply a partial update.
			auto libs = repo.listLibraries(req.path_params.at("id"));
			auto it = std::find_if(libs.begin(), libs.end(),
				[&lid](const MediaLibraryConfig& l){ return l.library_id == lid; });
			if (it == libs.end()) { route::err(res, 404, "library not found"); return; }
			std::string display_name      = b.value("display_name",      it->display_name);
			std::string library_type      = b.value("library_type",      it->library_type);
			std::string preferred_scraper = b.value("preferred_scraper", it->preferred_scraper);
			std::string preferred_language= b.value("preferred_language",it->preferred_language);
			bool        include_anidb     = b.value("include_anidb",     it->include_anidb);
			bool        skip_scraping     = b.value("skip_scraping",     it->skip_scraping);
			if (library_type != "show" && library_type != "movie" && library_type != "mixed") {
				route::err(res, 400, "library_type must be one of: show, movie, mixed");
				return;
			}
			repo.updateLibrary(lid, display_name, library_type, preferred_scraper, preferred_language,
			                   include_anidb, skip_scraping);
			// Skip-scraping just turned on — clear this library's existing
			// uncertain/unmatched backlog immediately rather than leaving it
			// to sit until an unrelated match pass happens to notice.
			if (skip_scraping && !it->skip_scraping)
				ContentRepository(db_).clearPendingMatchStateForLibrary(lid);
			if (b.contains("show_on_home"))
				repo.setLibraryShowOnHome(lid, b["show_on_home"].get<bool>());
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/sources/:id/libraries/:lid", e);
			route::err(res, 500, e.what());
		}
	});

	// GET /api/sources/:id/libraries/:lid/scraper-priority?item_type=show|movie
	svr.Get("/api/sources/:id/libraries/:lid/scraper-priority", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto lid = req.path_params.at("lid");
		auto item_type = req.has_param("item_type") ? req.get_param_value("item_type") : "show";
		if (item_type != "show" && item_type != "movie") { route::err(res, 400, "item_type must be show or movie"); return; }
		auto order = SourceRepository(db_).getScraperPriority(lid, item_type);
		route::ok(res, json{{"order", order}}.dump());
	});

	// PUT /api/sources/:id/libraries/:lid/scraper-priority  body {item_type, order:["tvdb","tmdb"]}
	svr.Put("/api/sources/:id/libraries/:lid/scraper-priority", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto lid = req.path_params.at("lid");
		try {
			auto b = json::parse(req.body);
			std::string item_type = b.value("item_type", "");
			if (item_type != "show" && item_type != "movie") { route::err(res, 400, "item_type must be show or movie"); return; }
			auto order = b.value("order", std::vector<std::string>{});
			SourceRepository(db_).setScraperPriority(lid, item_type, order);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("PUT /api/sources/:id/libraries/:lid/scraper-priority", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Delete("/api/sources/:id/libraries/:lid", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id  = req.path_params.at("id");
		auto lid = req.path_params.at("lid");
		try {
			SourceRepository(db_).removeLibrary(lid);
			logs_.push("[api] deleted library: " + lid + " (source: " + id + ")");
			route::ok(res, json{{"deleted", lid}}.dump());
		} catch (const std::exception& e) {
			route::logErr("DELETE /api/sources/" + id + "/libraries/" + lid, e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/sources/:id/sync", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		sync_.triggerSync(id);
		res.status = 202;
		route::ok(res, json{{"status","started"}, {"source_id", id}}.dump());
	});

	// Content-ingestion only — no orphan cleanup, no source-wide chapter/
	// specials rescans (see SyncManager::syncLibrary's doc comment). Picks up
	// new/changed content in this one library quickly; removals still need a
	// full source sync above to be noticed.
	svr.Post("/api/sources/:id/libraries/:lid/sync", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id  = req.path_params.at("id");
		auto lid = req.path_params.at("lid");
		sync_.triggerSync(id, lid);
		res.status = 202;
		route::ok(res, json{{"status","started"}, {"source_id", id}, {"library_id", lid}}.dump());
	});

	// Wipes this source's source_mapping first, so every item is re-resolved
	// as if this were its very first sync (fresh dedup, not trusting stale ids).
	svr.Post("/api/sources/:id/hard-sync", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto id = req.path_params.at("id");
		sync_.triggerHardSync(id);
		res.status = 202;
		route::ok(res, json{{"status","started"}, {"source_id", id}}.dump());
	});

	svr.Get("/api/sources/:id/browse/:kind", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto source_id = req.path_params.at("id");
		auto kind      = req.path_params.at("kind");
		auto src = sync_.findSource(source_id);
		if (!src) { route::err(res, 404, "source not found"); return; }

		std::vector<BrowseListItem> items;
		if (kind == "playlists") {
			items = src->browsePlaylists();
		} else if (kind == "collections") {
			std::string library_id = req.has_param("library_id") ? req.get_param_value("library_id") : "";
			if (library_id.empty()) { route::err(res, 400, "library_id required"); return; }
			auto ext_lib_id = SourceRepository(db_).getExternalLibId(library_id, source_id);
			if (ext_lib_id.empty()) { route::err(res, 404, "library not found for this source"); return; }
			items = src->browseCollections(ext_lib_id);
		} else {
			route::err(res, 400, "unknown kind: " + kind); return;
		}

		json result = json::array();
		for (const auto& item : items)
			result.push_back({{"id", item.id}, {"title", item.title}, {"item_count", item.item_count}});
		route::ok(res, result.dump());
	});

	svr.Get("/api/sources/:id/browse/:kind/:eid", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto source_id = req.path_params.at("id");
		auto kind      = req.path_params.at("kind");
		auto eid       = req.path_params.at("eid");
		auto src = sync_.findSource(source_id);
		if (!src) { route::err(res, 404, "source not found"); return; }

		std::vector<BrowseContentItem> items;
		if (kind == "playlists")
			items = src->browsePlaylistItems(eid);
		else if (kind == "collections")
			items = src->browseCollectionItems(eid);
		else { route::err(res, 400, "unknown kind: " + kind); return; }

		SourceRepository repo(db_);
		json result = json::array();
		for (const auto& item : items) {
			std::string kairos_id = repo.resolveKairosId(source_id, item.external_id, item.item_type);
			json entry = {
				{"item_type",   item.item_type},
				{"kairos_id",   kairos_id},
				{"title",       item.title},
				{"duration_ms", item.duration_ms},
				{"available",   !kairos_id.empty()},
			};
			if (item.item_type == "episode") {
				entry["show_title"] = item.show_title;
				if (item.season  >= 0) entry["season"]  = item.season;
				if (item.episode >= 0) entry["episode"] = item.episode;
			}
			result.push_back(std::move(entry));
		}
		route::ok(res, result.dump());
	});

	svr.Get("/api/sources/:id/sample-path", [this](const Req& req, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		auto sample = SourceRepository(db_).samplePath(req.path_params.at("id"));
		route::ok(res, json{{"path", sample ? json(*sample) : json(nullptr)}}.dump());
	});

	svr.Get("/api/sync/status", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		json j{{"running", sync_.isSyncing()}};
		auto current = sync_.currentSyncSourceId();
		if (!current.empty()) j["current_source_id"] = current;
		route::ok(res, j.dump());
	});
}
