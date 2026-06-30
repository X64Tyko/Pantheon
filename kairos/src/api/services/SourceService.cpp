#include "SourceService.h"
#include "../RouteHelpers.h"
#include "../../db/SourceRepository.h"
#include "../../log/LogBuffer.h"
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
	: db_(ctx.db), sync_(ctx.sync), logs_(ctx.logs) {}

void SourceService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/sources/types", [](const Req&, Res& res) {
		json types = {
			{{"type","plex"},     {"display_name","Plex"},        {"supported",true}},
			{{"type","jellyfin"}, {"display_name","Jellyfin"},    {"supported",true}},
			{{"type","emby"},     {"display_name","Emby"},        {"supported",true}},
			{{"type","local"},    {"display_name","Local Media"}, {"supported",true}},
		};
		route::ok(res, types.dump());
	});

	svr.Get("/api/sources", [this](const Req&, Res& res) {
		json result = json::array();
		for (const auto& s : SourceRepository(db_).listSources())
			result.push_back({{"source_id", s.source_id}, {"source_type", s.source_type},
			                  {"display_name", s.display_name}, {"base_url", s.base_url},
			                  {"enabled", s.enabled}});
		route::ok(res, result.dump());
	});

	svr.Post("/api/sources", [this](const Req& req, Res& res) {
		try {
			auto b = json::parse(req.body);
			std::string source_id    = b.value("source_id",    "");
			std::string source_type  = b.value("source_type",  "");
			std::string display_name = b.value("display_name", "");
			std::string base_url     = b.value("base_url",     "");
			if (source_id.empty() || source_type.empty() || display_name.empty()) {
				route::err(res, 400, "source_id, source_type, and display_name required");
				return;
			}
			SourceRepository(db_).createSource(source_id, source_type, display_name, base_url);
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

	svr.Get("/api/sources/:id/libraries/available", [this](const Req& req, Res& res) {
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
		auto source_id = req.path_params.at("id");
		json result = json::array();
		for (const auto& lib : SourceRepository(db_).listLibraries(source_id))
			result.push_back({{"library_id", lib.library_id}, {"source_id", lib.source_id},
			                  {"external_lib_id", lib.external_lib_id},
			                  {"display_name", lib.display_name}, {"library_type", lib.library_type},
			                  {"preferred_scraper", lib.preferred_scraper},
			                  {"preferred_language", lib.preferred_language},
			                  {"enabled", lib.enabled}});
		route::ok(res, result.dump());
	});

	svr.Post("/api/sources/:id/libraries", [this](const Req& req, Res& res) {
		try {
			auto source_id = req.path_params.at("id");
			auto b = json::parse(req.body);
			std::string external_lib_id    = b.value("external_lib_id",    "");
			std::string display_name       = b.value("display_name",       "");
			std::string library_type       = b.value("library_type",       "show");
			std::string preferred_scraper  = b.value("preferred_scraper",  "");
			std::string preferred_language = b.value("preferred_language", "");
			if (external_lib_id.empty() || display_name.empty()) {
				route::err(res, 400, "external_lib_id and display_name required"); return;
			}
			std::string library_id = SourceRepository(db_).createLibrary(
				source_id, external_lib_id, display_name, library_type,
				preferred_scraper, preferred_language);
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
			std::string preferred_scraper = b.value("preferred_scraper", it->preferred_scraper);
			std::string preferred_language= b.value("preferred_language",it->preferred_language);
			repo.updateLibrary(lid, display_name, preferred_scraper, preferred_language);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const json::exception& e) {
			route::err(res, 400, e.what());
		} catch (const std::exception& e) {
			route::logErr("PATCH /api/sources/:id/libraries/:lid", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Delete("/api/sources/:id/libraries/:lid", [this](const Req& req, Res& res) {
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
		auto id = req.path_params.at("id");
		sync_.triggerSync(id);
		res.status = 202;
		route::ok(res, json{{"status","started"}, {"source_id", id}}.dump());
	});

	svr.Get("/api/sources/:id/browse/:kind", [this](const Req& req, Res& res) {
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
		auto sample = SourceRepository(db_).samplePath(req.path_params.at("id"));
		route::ok(res, json{{"path", sample ? json(*sample) : json(nullptr)}}.dump());
	});

	svr.Get("/api/sync/status", [this](const Req&, Res& res) {
		route::ok(res, json{{"running", sync_.isSyncing()}}.dump());
	});
}
