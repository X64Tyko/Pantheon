#include "Router.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include "download/DownloadManager.h"
#include "log/LogBuffer.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "sync/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <ctime>
#include <random>
#include <set>
#include <sstream>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

std::string generateId() {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << dist(rng);
    return ss.str();
}

// Split on ';', trim each token, skip empty parts
std::vector<std::string> splitSemicolon(const std::string& s) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ';')) {
        auto b = token.find_first_not_of(" \t");
        auto e = token.find_last_not_of(" \t");
        if (b != std::string::npos) parts.push_back(token.substr(b, e - b + 1));
    }
    return parts;
}

// Build an IN(?,?,...) clause and append bound values; returns true if non-empty
bool appendInClause(const std::string& col, const std::string& raw,
                    std::string& extras, std::vector<std::string>& vals) {
    auto parts = splitSemicolon(raw);
    if (parts.empty()) return false;
    if (parts.size() == 1) {
        extras += " AND " + col + " = ?";
    } else {
        std::string ph; for (size_t i = 0; i < parts.size(); ++i) ph += (i ? ",?" : "?");
        extras += " AND " + col + " IN (" + ph + ")";
    }
    for (auto& p : parts) vals.push_back(p);
    return true;
}

// Build an EXISTS(json_each IN ...) clause for a JSON-array column
bool appendJsonInClause(const std::string& tbl, const std::string& col, const std::string& raw,
                        std::string& extras, std::vector<std::string>& vals) {
    auto parts = splitSemicolon(raw);
    if (parts.empty()) return false;
    if (parts.size() == 1) {
        extras += " AND EXISTS (SELECT 1 FROM json_each(" + tbl + "." + col + ")"
                  " WHERE json_each.value = ?)";
    } else {
        std::string ph; for (size_t i = 0; i < parts.size(); ++i) ph += (i ? ",?" : "?");
        extras += " AND EXISTS (SELECT 1 FROM json_each(" + tbl + "." + col + ")"
                  " WHERE json_each.value IN (" + ph + "))";
    }
    for (auto& p : parts) vals.push_back(p);
    return true;
}

} // namespace

void Router::ok(Res& res, const std::string& body) {
    res.set_content(body, "application/json");
}

void Router::err(Res& res, int status, const std::string& msg) {
    res.status = status;
    res.set_content(json{{"error", msg}}.dump(), "application/json");
}

void Router::clearScheduleCache(const std::string& channel_id) {
    auto now = static_cast<int64_t>(std::time(nullptr));

    SQLite::Statement d1(db_.get(),
        "DELETE FROM scheduled_program WHERE channel_id = ?");
    d1.bind(1, channel_id);
    d1.exec();

    // Remove future EPG-scheduled play_history entries so the next project()
    // call starts from a clean slate rather than replaying phantom airings.
    SQLite::Statement d2(db_.get(),
        "DELETE FROM play_history WHERE channel_id = ? AND is_scheduled = 1 AND aired_at >= ?");
    d2.bind(1, channel_id);
    d2.bind(2, now);
    d2.exec();
}

// ---------------------------------------------------------------------------

Router::Router(httplib::Server& svr, Database& db, SyncManager& sync,
               ConfStore& conf, LogBuffer& logs,
               RuleEngine& engine, EPGMaterializer& materializer,
               DownloadManager& dl)
    : svr_(svr), db_(db), sync_(sync), conf_(conf), logs_(logs),
      engine_(engine), materializer_(materializer), dl_(dl)
{}

static void logErr(const std::string& ctx, const std::exception& e) {
    std::cerr << "[error] " << ctx << ": " << e.what() << "\n";
}

static bool isValidTimezone(const std::string& tz) {
    if (tz.empty()) return false;
    try { std::chrono::locate_zone(tz); return true; }
    catch (...) { return false; }
}

void Router::registerRoutes() {
    // CORS preflight — allows curl/Postman during dev without the Vite proxy
    svr_.Options(".*", [](const Req&, Res& res) {
        res.set_header("Access-Control-Allow-Origin",  "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    svr_.set_post_routing_handler([](const Req&, Res& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    // Hot-reload kairos.conf on every request so UI credential changes take effect immediately
    svr_.set_pre_routing_handler([this](const Req&, Res&) -> httplib::Server::HandlerResponse {
        conf_.maybeReload();
        return httplib::Server::HandlerResponse::Unhandled;
    });

    registerSourceRoutes();
    registerConfigRoutes();
    registerChannelRoutes();
    registerBlockRoutes();
    registerContentRoutes();
    registerPlaylistRoutes();
    registerFillerRoutes();
    registerActivityRoutes();
    registerDownloadRoutes();
    registerSchedulerRoutes();

    // Sync status
    svr_.Get("/api/sync/status", [this](const Req&, Res& res) {
        ok(res, json{{"running", sync_.isSyncing()}}.dump());
    });

    // Serve built Hades UI — SPA fallback so client-side routes work
    svr_.set_mount_point("/", "./ui-dist");
    svr_.Get(".*", [](const Req&, Res& res) {
        std::ifstream ifs("./ui-dist/index.html");
        if (!ifs) { res.status = 404; return; }
        std::string html((std::istreambuf_iterator<char>(ifs)), {});
        res.set_content(html, "text/html; charset=utf-8");
    });
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

void Router::registerSourceRoutes() {

    // Static list of known source types — UI uses this to disable unsupported ones
    svr_.Get("/api/sources/types", [](const Req&, Res& res) {
        json types = {
            {{"type","plex"},     {"display_name","Plex"},        {"supported",true}},
            {{"type","jellyfin"}, {"display_name","Jellyfin"},    {"supported",false}},
            {{"type","emby"},     {"display_name","Emby"},        {"supported",false}},
            {{"type","local"},    {"display_name","Local Media"}, {"supported",false}},
        };
        ok(res, types.dump());
    });

    svr_.Get("/api/sources", [this](const Req&, Res& res) {
        SQLite::Statement q(db_.get(),
            "SELECT source_id, source_type, display_name, COALESCE(base_url,''), enabled "
            "FROM media_source ORDER BY display_name");
        json result = json::array();
        while (q.executeStep()) {
            result.push_back({
                {"source_id",    q.getColumn(0).getString()},
                {"source_type",  q.getColumn(1).getString()},
                {"display_name", q.getColumn(2).getString()},
                {"base_url",     q.getColumn(3).getString()},
                {"enabled",      q.getColumn(4).getInt() != 0},
            });
        }
        ok(res, result.dump());
    });

    svr_.Post("/api/sources", [this](const Req& req, Res& res) {
        try {
            auto b = json::parse(req.body);
            std::string source_id    = b.value("source_id", "");
            std::string source_type  = b.value("source_type", "");
            std::string display_name = b.value("display_name", "");
            std::string base_url     = b.value("base_url", "");
            if (source_id.empty() || source_type.empty() || display_name.empty()) {
                err(res, 400, "source_id, source_type, and display_name required");
                return;
            }
            SQLite::Statement s(db_.get(),
                "INSERT INTO media_source (source_id, source_type, display_name, base_url) "
                "VALUES (?,?,?,?)");
            s.bind(1, source_id); s.bind(2, source_type);
            s.bind(3, display_name); s.bind(4, base_url);
            s.exec();
            sync_.loadSources(); // pick up new source immediately
            res.status = 201;
            ok(res, json{{"source_id", source_id}}.dump());
        } catch (const SQLite::Exception& e) {
            err(res, 409, e.what()); // likely UNIQUE constraint
        } catch (const json::exception& e) {
            err(res, 400, e.what());
        }
    });

    // Test a source connection without persisting anything
    svr_.Post("/api/sources/test", [](const Req& req, Res& res) {
        try {
            auto b = json::parse(req.body);
            std::string source_type = b.value("source_type", "");
            std::string base_url    = b.value("base_url", "");
            std::string token       = b.value("token", "");

            if (source_type != "plex") {
                err(res, 400, "connection test not yet supported for " + source_type);
                return;
            }
            if (base_url.empty() || token.empty()) {
                err(res, 400, "base_url and token are required"); return;
            }

            httplib::Client client(base_url);
            client.set_default_headers({
                {"X-Plex-Token", token},
                {"Accept",       "application/json"}
            });
            client.set_connection_timeout(10);
            client.set_read_timeout(10);

            auto r = client.Get("/library/sections");
            if (!r) {
                ok(res, json{{"ok", false},
                    {"error", "Cannot connect to " + base_url + ": " +
                              httplib::to_string(r.error())}}.dump());
                return;
            }
            if (r->status == 401 || r->status == 403) {
                ok(res, json{{"ok", false},
                    {"error", "Authentication failed — check your Plex token"}}.dump());
                return;
            }
            if (r->status != 200) {
                ok(res, json{{"ok", false},
                    {"error", "Unexpected response: HTTP " + std::to_string(r->status)}}.dump());
                return;
            }
            ok(res, json{{"ok", true}}.dump());
        } catch (const json::exception& e) {
            err(res, 400, e.what());
        } catch (const std::exception& e) {
            ok(res, json{{"ok", false}, {"error", std::string(e.what())}}.dump());
        }
    });

    svr_.Delete("/api/sources/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement s(db_.get(), "DELETE FROM media_source WHERE source_id = ?");
        s.bind(1, id);
        s.exec();
        sync_.loadSources();
        ok(res, json{{"deleted", id}}.dump());
    });

    // List libraries available on the live server (not what's configured — what's there)
    svr_.Get("/api/sources/:id/libraries/available", [this](const Req& req, Res& res) {
        auto id  = req.path_params.at("id");
        auto src = sync_.findSource(id);
        if (!src)            { err(res, 404, "source not found or not loaded"); return; }
        if (!src->isSupported()) { err(res, 501, src->sourceType() + " not yet supported"); return; }
        auto libs = src->listAvailableLibraries();
        json result = json::array();
        for (const auto& lib : libs)
            result.push_back({{"external_lib_id", lib.external_lib_id},
                              {"name", lib.name}, {"type", lib.type}});
        ok(res, result.dump());
    });

    svr_.Get("/api/sources/:id/libraries", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement q(db_.get(),
            "SELECT library_id, external_lib_id, display_name, library_type, enabled "
            "FROM media_library WHERE source_id = ? ORDER BY display_name");
        q.bind(1, id);
        json result = json::array();
        while (q.executeStep()) {
            result.push_back({
                {"library_id",      q.getColumn(0).getString()},
                {"source_id",       id},
                {"external_lib_id", q.getColumn(1).getString()},
                {"display_name",    q.getColumn(2).getString()},
                {"library_type",    q.getColumn(3).getString()},
                {"enabled",         q.getColumn(4).getInt() != 0},
            });
        }
        ok(res, result.dump());
    });

    svr_.Post("/api/sources/:id/libraries", [this](const Req& req, Res& res) {
        try {
            auto source_id = req.path_params.at("id");
            auto b = json::parse(req.body);
            std::string external_lib_id = b.value("external_lib_id", "");
            std::string display_name    = b.value("display_name", "");
            std::string library_type    = b.value("library_type", "show");
            if (external_lib_id.empty() || display_name.empty()) {
                err(res, 400, "external_lib_id and display_name required"); return;
            }
            std::string library_id = generateId();
            SQLite::Statement s(db_.get(),
                "INSERT INTO media_library "
                "(library_id, source_id, external_lib_id, display_name, library_type) "
                "VALUES (?,?,?,?,?)");
            s.bind(1, library_id); s.bind(2, source_id);
            s.bind(3, external_lib_id); s.bind(4, display_name);
            s.bind(5, library_type);
            s.exec();
            res.status = 201;
            ok(res, json{{"library_id", library_id}}.dump());
        } catch (const SQLite::Exception& e) {
            err(res, 409, e.what());
        } catch (const json::exception& e) {
            err(res, 400, e.what());
        }
    });

    svr_.Delete("/api/sources/:id/libraries/:lid", [this](const Req& req, Res& res) {
        auto lid = req.path_params.at("lid");
        SQLite::Statement s(db_.get(), "DELETE FROM media_library WHERE library_id = ?");
        s.bind(1, lid);
        s.exec();
        ok(res, json{{"deleted", lid}}.dump());
    });

    svr_.Post("/api/sources/:id/sync", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        sync_.triggerSync(id);
        res.status = 202;
        ok(res, json{{"status","started"}, {"source_id", id}}.dump());
    });

    // ── Plex browse: playlists / collections ──────────────────────────────────
    // These call the Plex server live — they do not use synced data.

    // Helper: build a temporary Plex httplib client for the given source.
    // Returns empty string in base_url if source is not found or not plex.
    auto makePlexClient = [this](const std::string& source_id,
                                  std::string& base_url_out,
                                  std::unique_ptr<httplib::Client>& client_out) -> bool {
        std::string source_type;
        SQLite::Statement sq(db_.get(),
            "SELECT base_url, source_type FROM media_source WHERE source_id = ?");
        sq.bind(1, source_id);
        if (!sq.executeStep()) return false;
        base_url_out = sq.getColumn(0).getString();
        source_type  = sq.getColumn(1).getString();
        if (source_type != "plex" || base_url_out.empty()) return false;
        std::string token = conf_.token(source_id);
        client_out = std::make_unique<httplib::Client>(base_url_out);
        client_out->set_default_headers({{"X-Plex-Token", token}, {"Accept", "application/json"}});
        client_out->set_connection_timeout(10);
        client_out->set_read_timeout(30);
        return true;
    };

    // Helper: resolve a Plex ratingKey to an internal kairos_id.
    auto resolveKairosId = [this](const std::string& source_id,
                                   const std::string& external_id,
                                   const std::string& item_type) -> std::string {
        SQLite::Statement lk(db_.get(),
            "SELECT kairos_id FROM source_mapping "
            "WHERE source_id = ? AND external_id = ? AND item_type = ?");
        lk.bind(1, source_id); lk.bind(2, external_id); lk.bind(3, item_type);
        return lk.executeStep() ? lk.getColumn(0).getString() : "";
    };

    // GET /api/sources/:id/browse/playlists — all Plex playlists on the server
    svr_.Get("/api/sources/:id/browse/playlists", [this, makePlexClient](const Req& req, Res& res) {
        auto source_id = req.path_params.at("id");
        std::string base_url;
        std::unique_ptr<httplib::Client> client;
        if (!makePlexClient(source_id, base_url, client)) {
            err(res, 400, "source not found or not a Plex source"); return;
        }
        auto r = client->Get("/playlists?playlistType=video");
        if (!r || r->status != 200) { err(res, 502, "failed to fetch playlists from Plex"); return; }
        json result = json::array();
        try {
            auto j = json::parse(r->body);
            const auto& md = j["MediaContainer"];
            if (md.contains("Metadata")) {
                for (const auto& pl : md["Metadata"]) {
                    result.push_back({
                        {"id",         pl["ratingKey"].get<std::string>()},
                        {"title",      pl.value("title", "")},
                        {"item_count", pl.value("leafCount", 0)},
                    });
                }
            }
        } catch (...) { err(res, 502, "failed to parse Plex playlist response"); return; }
        ok(res, result.dump());
    });

    // GET /api/sources/:id/browse/playlists/:plid/items
    svr_.Get("/api/sources/:id/browse/playlists/:plid/items",
             [this, makePlexClient, resolveKairosId](const Req& req, Res& res) {
        auto source_id = req.path_params.at("id");
        auto plid      = req.path_params.at("plid");
        std::string base_url;
        std::unique_ptr<httplib::Client> client;
        if (!makePlexClient(source_id, base_url, client)) {
            err(res, 400, "source not found or not a Plex source"); return;
        }
        auto r = client->Get("/playlists/" + plid + "/items");
        if (!r || r->status != 200) { err(res, 502, "failed to fetch playlist items from Plex"); return; }
        json result = json::array();
        try {
            auto j = json::parse(r->body);
            const auto& md = j["MediaContainer"];
            if (md.contains("Metadata")) {
                for (const auto& item : md["Metadata"]) {
                    std::string plex_type  = item.value("type", "");
                    std::string item_type  = (plex_type == "movie") ? "movie" : "episode";
                    std::string rating_key = item["ratingKey"].get<std::string>();
                    std::string kairos_id  = resolveKairosId(source_id, rating_key, item_type);
                    json entry = {
                        {"item_type",   item_type},
                        {"kairos_id",   kairos_id},
                        {"title",       item.value("title", "")},
                        {"duration_ms", item.value("duration", int64_t{0})},
                        {"available",   !kairos_id.empty()},
                    };
                    if (plex_type == "episode") {
                        entry["show_title"] = item.value("grandparentTitle", "");
                        if (item.contains("parentIndex")) entry["season"]  = item["parentIndex"].get<int>();
                        if (item.contains("index"))       entry["episode"] = item["index"].get<int>();
                    }
                    result.push_back(std::move(entry));
                }
            }
        } catch (...) { err(res, 502, "failed to parse Plex playlist items response"); return; }
        ok(res, result.dump());
    });

    // GET /api/sources/:id/browse/collections?library_id=<kairos library_id>
    svr_.Get("/api/sources/:id/browse/collections",
             [this, makePlexClient](const Req& req, Res& res) {
        auto source_id = req.path_params.at("id");
        std::string library_id = req.has_param("library_id") ? req.get_param_value("library_id") : "";
        if (library_id.empty()) { err(res, 400, "library_id required"); return; }

        // Look up the external_lib_id for this kairos library_id
        std::string ext_lib_id;
        SQLite::Statement lq(db_.get(),
            "SELECT external_lib_id FROM media_library WHERE library_id = ? AND source_id = ?");
        lq.bind(1, library_id); lq.bind(2, source_id);
        if (!lq.executeStep()) { err(res, 404, "library not found for this source"); return; }
        ext_lib_id = lq.getColumn(0).getString();

        std::string base_url;
        std::unique_ptr<httplib::Client> client;
        if (!makePlexClient(source_id, base_url, client)) {
            err(res, 400, "source not found or not a Plex source"); return;
        }
        auto r = client->Get("/library/sections/" + ext_lib_id + "/collections");
        if (!r || r->status != 200) { err(res, 502, "failed to fetch collections from Plex"); return; }
        json result = json::array();
        try {
            auto j = json::parse(r->body);
            const auto& md = j["MediaContainer"];
            if (md.contains("Metadata")) {
                for (const auto& col : md["Metadata"]) {
                    result.push_back({
                        {"id",         col["ratingKey"].get<std::string>()},
                        {"title",      col.value("title", "")},
                        {"item_count", col.value("childCount", 0)},
                    });
                }
            }
        } catch (...) { err(res, 502, "failed to parse Plex collections response"); return; }
        ok(res, result.dump());
    });

    // GET /api/sources/:id/browse/collections/:cid/items
    svr_.Get("/api/sources/:id/browse/collections/:cid/items",
             [this, makePlexClient, resolveKairosId](const Req& req, Res& res) {
        auto source_id = req.path_params.at("id");
        auto cid       = req.path_params.at("cid");
        std::string base_url;
        std::unique_ptr<httplib::Client> client;
        if (!makePlexClient(source_id, base_url, client)) {
            err(res, 400, "source not found or not a Plex source"); return;
        }
        auto r = client->Get("/library/metadata/" + cid + "/children");
        if (!r || r->status != 200) { err(res, 502, "failed to fetch collection items from Plex"); return; }
        json result = json::array();
        try {
            auto j = json::parse(r->body);
            const auto& md = j["MediaContainer"];
            if (md.contains("Metadata")) {
                for (const auto& item : md["Metadata"]) {
                    std::string plex_type  = item.value("type", "");
                    std::string item_type  = (plex_type == "movie") ? "movie" : "episode";
                    std::string rating_key = item["ratingKey"].get<std::string>();
                    std::string kairos_id  = resolveKairosId(source_id, rating_key, item_type);
                    json entry = {
                        {"item_type",   item_type},
                        {"kairos_id",   kairos_id},
                        {"title",       item.value("title", "")},
                        {"duration_ms", item.value("duration", int64_t{0})},
                        {"available",   !kairos_id.empty()},
                    };
                    if (plex_type == "episode") {
                        entry["show_title"] = item.value("grandparentTitle", "");
                        if (item.contains("parentIndex")) entry["season"]  = item["parentIndex"].get<int>();
                        if (item.contains("index"))       entry["episode"] = item["index"].get<int>();
                    }
                    result.push_back(std::move(entry));
                }
            }
        } catch (...) { err(res, 502, "failed to parse Plex collection items response"); return; }
        ok(res, result.dump());
    });
}

// ---------------------------------------------------------------------------
// Config — credential management (reads/writes kairos.conf)
// ---------------------------------------------------------------------------

void Router::registerConfigRoutes() {

    // Credential status for all configured sources
    svr_.Get("/api/config/credentials", [this](const Req&, Res& res) {
        SQLite::Statement q(db_.get(),
            "SELECT source_id, source_type, display_name FROM media_source ORDER BY display_name");
        json result = json::array();
        while (q.executeStep()) {
            auto sid = q.getColumn(0).getString();
            result.push_back({
                {"source_id",    sid},
                {"source_type",  q.getColumn(1).getString()},
                {"display_name", q.getColumn(2).getString()},
                {"has_token",    conf_.hasToken(sid)},
                {"has_user_id",  conf_.hasUserId(sid)},
            });
        }
        ok(res, result.dump());
    });

    svr_.Get("/api/config/credentials/:source_id", [this](const Req& req, Res& res) {
        auto sid = req.path_params.at("source_id");
        ok(res, json{{"has_token",   conf_.hasToken(sid)},
                     {"has_user_id", conf_.hasUserId(sid)}}.dump());
    });

    svr_.Put("/api/config/credentials/:source_id", [this](const Req& req, Res& res) {
        try {
            auto sid    = req.path_params.at("source_id");
            auto b      = json::parse(req.body);
            auto token   = b.value("token",   "");
            auto user_id = b.value("user_id", "");
            conf_.setCredentials(sid, token, user_id);
            sync_.loadSources();
            ok(res, json{{"ok", true}}.dump());
        } catch (const json::exception& e) {
            err(res, 400, e.what());
        }
    });

    svr_.Delete("/api/config/credentials/:source_id", [this](const Req& req, Res& res) {
        auto sid = req.path_params.at("source_id");
        conf_.removeSource(sid);
        sync_.loadSources();
        ok(res, json{{"ok", true}}.dump());
    });

    svr_.Get("/api/config/path-maps/:source_id", [this](const Req& req, Res& res) {
        auto sid  = req.path_params.at("source_id");
        auto maps = conf_.getPathMaps(sid);
        json result = json::array();
        for (const auto& [from, to] : maps)
            result.push_back({{"from", from}, {"to", to}});
        ok(res, result.dump());
    });

    svr_.Put("/api/config/path-maps/:source_id", [this](const Req& req, Res& res) {
        try {
            auto sid = req.path_params.at("source_id");
            auto b   = json::parse(req.body);
            std::vector<std::pair<std::string,std::string>> maps;
            if (b.contains("maps") && b["maps"].is_array()) {
                for (const auto& m : b["maps"]) {
                    auto from = m.value("from", "");
                    auto to   = m.value("to",   "");
                    if (!from.empty()) maps.push_back({from, to});
                }
            }
            conf_.setPathMaps(sid, maps);
            ok(res, json{{"ok", true}}.dump());
        } catch (const json::exception& e) { err(res, 400, e.what()); }
    });

    // Return one raw (pre-mapping) file path from this source so the user can
    // see the actual prefix they need to map from.
    svr_.Get("/api/sources/:id/sample-path", [this](const Req& req, Res& res) {
        auto source_id = req.path_params.at("id");
        std::string sample;
        SQLite::Statement q(db_.get(), R"(
            SELECT e.file_path FROM episode e
            JOIN source_mapping sm ON sm.kairos_id = e.episode_id AND sm.item_type = 'episode'
            JOIN media_library ml ON ml.library_id = sm.library_id
            WHERE ml.source_id = ? AND e.file_path != ''
            LIMIT 1
        )");
        q.bind(1, source_id);
        if (q.executeStep()) {
            sample = q.getColumn(0).getString();
        } else {
            SQLite::Statement mq(db_.get(), R"(
                SELECT m.file_path FROM movie m
                JOIN source_mapping sm ON sm.kairos_id = m.movie_id AND sm.item_type = 'movie'
                JOIN media_library ml ON ml.library_id = sm.library_id
                WHERE ml.source_id = ? AND m.file_path != ''
                LIMIT 1
            )");
            mq.bind(1, source_id);
            if (mq.executeStep()) sample = mq.getColumn(0).getString();
        }
        if (sample.empty())
            ok(res, json{{"path", nullptr}}.dump());
        else
            ok(res, json{{"path", sample}}.dump());
    });
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

void Router::registerChannelRoutes() {

    svr_.Get("/api/channels", [this](const Req&, Res& res) {
      try {
        SQLite::Statement q(db_.get(),
            "SELECT channel_id, name, number, timezone, default_filler_selection, seed, advance_mode, "
            "       offline_video_path, offline_image_path, offline_audio_id, offline_audio_type, "
            "       offline_audio_title, logo_path "
            "FROM channel ORDER BY number");
        json result = json::array();
        while (q.executeStep()) {
            std::string cid = q.getColumn(0).getString();
            json channel = {
                {"channel_id",               cid},
                {"name",                     q.getColumn(1).getString()},
                {"number",                   q.getColumn(2).getInt()},
                {"timezone",                 q.getColumn(3).getString()},
                {"default_filler_selection", q.getColumn(4).getString()},
                {"seed",                     q.getColumn(5).getInt()},
                {"advance_mode",             q.getColumn(6).getString()},
                {"offline_video_path",        q.getColumn(7).getString()},
                {"offline_image_path",        q.getColumn(8).getString()},
                {"offline_audio_id",          q.getColumn(9).getString()},
                {"offline_audio_type",        q.getColumn(10).getString()},
                {"offline_audio_title",       q.getColumn(11).getString()},
                {"logo_path",                 q.getColumn(12).getString()},
            };
            SQLite::Statement fq(db_.get(), R"(
                SELECT cfe.id, cfe.filler_list_id, fl.title,
                       cfe.advancement, cfe.weight, cfe.position
                FROM channel_filler_entry cfe
                JOIN filler_list fl ON fl.filler_list_id = cfe.filler_list_id
                WHERE cfe.channel_id = ? ORDER BY cfe.position
            )");
            fq.bind(1, cid);
            json filler_entries = json::array();
            while (fq.executeStep()) {
                filler_entries.push_back({
                    {"id",             fq.getColumn(0).getInt()},
                    {"filler_list_id", fq.getColumn(1).getString()},
                    {"title",          fq.getColumn(2).getString()},
                    {"advancement",    fq.getColumn(3).getString()},
                    {"weight",         fq.getColumn(4).getInt()},
                    {"position",       fq.getColumn(5).getInt()},
                });
            }
            channel["default_filler_entries"] = filler_entries;
            result.push_back(channel);
        }
        ok(res, result.dump());
      } catch (const std::exception& e) {
        logErr("GET /api/channels", e); err(res, 500, e.what());
      }
    });

    svr_.Post("/api/channels", [this](const Req& req, Res& res) {
        try {
            auto b = json::parse(req.body);
            std::string name         = b.value("name", "");
            int         number       = b.value("number", 0);
            std::string timezone     = b.value("timezone", "UTC");
            std::string advance_mode = b.value("advance_mode", "scheduled");
            if (name.empty() || number == 0) {
                err(res, 400, "name and number required"); return;
            }
            if (!isValidTimezone(timezone)) {
                err(res, 400, ("invalid timezone: " + timezone).c_str()); return;
            }
            std::string channel_id = generateId();
            SQLite::Statement s(db_.get(),
                "INSERT INTO channel (channel_id, name, number, timezone, advance_mode) VALUES (?,?,?,?,?)");
            s.bind(1, channel_id); s.bind(2, name);
            s.bind(3, number);     s.bind(4, timezone); s.bind(5, advance_mode);
            s.exec();
            res.status = 201;
            ok(res, json{{"channel_id", channel_id}}.dump());
        } catch (const SQLite::Exception& e) {
            err(res, 409, e.what());
        } catch (const json::exception& e) {
            err(res, 400, e.what());
        }
    });

    svr_.Patch("/api/channels/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            auto upd = [&](const char* col, const std::string& val) {
                SQLite::Statement s(db_.get(),
                    std::string("UPDATE channel SET ") + col + " = ? WHERE channel_id = ?");
                s.bind(1, val); s.bind(2, id); s.exec();
            };
            if (b.contains("name"))                     upd("name",                     b["name"]);
            if (b.contains("timezone")) {
                std::string tz = b["timezone"].get<std::string>();
                if (!isValidTimezone(tz)) { err(res, 400, ("invalid timezone: " + tz).c_str()); return; }
                upd("timezone", tz);
            }
            if (b.contains("number")) {
                SQLite::Statement s(db_.get(), "UPDATE channel SET number = ? WHERE channel_id = ?");
                s.bind(1, b["number"].get<int>()); s.bind(2, id); s.exec();
            }
            if (b.contains("default_filler_selection")) upd("default_filler_selection", b["default_filler_selection"]);
            if (b.contains("advance_mode"))             upd("advance_mode",             b["advance_mode"]);
            if (b.contains("offline_video_path"))       upd("offline_video_path",       b["offline_video_path"]);
            if (b.contains("offline_image_path"))       upd("offline_image_path",       b["offline_image_path"]);
            if (b.contains("offline_audio_id"))          upd("offline_audio_id",          b["offline_audio_id"]);
            if (b.contains("offline_audio_type"))        upd("offline_audio_type",        b["offline_audio_type"]);
            if (b.contains("offline_audio_title"))       upd("offline_audio_title",       b["offline_audio_title"]);
            if (b.contains("logo_path"))                upd("logo_path",                b["logo_path"]);
            if (b.contains("seed")) {
                SQLite::Statement s(db_.get(), "UPDATE channel SET seed = ? WHERE channel_id = ?");
                s.bind(1, b["seed"].get<int>()); s.bind(2, id); s.exec();
            }
            // These changes alter what gets scheduled.
            if (b.contains("timezone") || b.contains("seed") || b.contains("advance_mode"))
                clearScheduleCache(id);
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Delete("/api/channels/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        try {
            SQLite::Statement s(db_.get(), "DELETE FROM channel WHERE channel_id = ?");
            s.bind(1, id);
            s.exec();
            ok(res, json{{"deleted", id}}.dump());
        } catch (const std::exception& e) { err(res, 500, e.what()); }
    });

    // ── Export / Import ───────────────────────────────────────────────────────

    svr_.Get("/api/channels/:id/export", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        std::string depth = "shallow";
        if (req.has_param("depth")) depth = req.get_param_value("depth");
        bool deep = (depth == "deep");
        try {
            SQLite::Statement cq(db_.get(), R"(
                SELECT name, number, timezone, advance_mode, default_filler_selection, seed
                FROM channel WHERE channel_id = ?
            )");
            cq.bind(1, channel_id);
            if (!cq.executeStep()) { err(res, 404, "channel not found"); return; }

            json channel_j = {
                {"name",                     cq.getColumn(0).getString()},
                {"number",                   cq.getColumn(1).getInt()},
                {"timezone",                 cq.getColumn(2).getString()},
                {"advance_mode",             cq.getColumn(3).getString()},
                {"default_filler_selection", cq.getColumn(4).getString()},
            };
            if (!cq.getColumn(5).isNull()) channel_j["seed"] = cq.getColumn(5).getInt();

            SQLite::Statement cfq(db_.get(), R"(
                SELECT fl.title, cfe.advancement, cfe.weight
                FROM channel_filler_entry cfe
                JOIN filler_list fl ON fl.filler_list_id = cfe.filler_list_id
                WHERE cfe.channel_id = ? ORDER BY cfe.position
            )");
            cfq.bind(1, channel_id);
            json ch_filler = json::array();
            while (cfq.executeStep()) {
                ch_filler.push_back({
                    {"title",       cfq.getColumn(0).getString()},
                    {"advancement", cfq.getColumn(1).getString()},
                    {"weight",      cfq.getColumn(2).getInt()},
                });
            }
            channel_j["default_filler_entries"] = ch_filler;

            SQLite::Statement bq(db_.get(), R"(
                SELECT block_id, name, block_type, day_mask, start_time, end_time,
                       program_count, priority, max_content_rating, advancement, cursor_scope,
                       late_start_mins, align_to_mins, inter_filler, early_start_secs,
                       filler_selection, smart_pct, start_scope, no_history_behavior,
                       max_consecutive_episodes
                FROM block WHERE channel_id = ?
                ORDER BY start_time, priority DESC
            )");
            bq.bind(1, channel_id);

            json blocks = json::array();
            while (bq.executeStep()) {
                std::string bid = bq.getColumn(0).getString();
                json block_j = {
                    {"name",                     bq.getColumn(1).getString()},
                    {"block_type",               bq.getColumn(2).getString()},
                    {"day_mask",                 bq.getColumn(3).getInt()},
                    {"start_time",               bq.getColumn(4).getString()},
                    {"program_count",            bq.getColumn(6).getInt()},
                    {"priority",                 bq.getColumn(7).getInt()},
                    {"max_content_rating",       bq.getColumn(8).getString()},
                    {"advancement",              bq.getColumn(9).getString()},
                    {"cursor_scope",             bq.getColumn(10).getString()},
                    {"late_start_mins",          bq.getColumn(11).getInt()},
                    {"align_to_mins",            bq.getColumn(12).getInt()},
                    {"inter_filler",             bq.getColumn(13).getInt() != 0},
                    {"early_start_secs",         bq.getColumn(14).getInt()},
                    {"filler_selection",         bq.getColumn(15).getString()},
                    {"smart_pct",                bq.getColumn(16).getInt()},
                    {"start_scope",              bq.getColumn(17).getString()},
                    {"no_history_behavior",      bq.getColumn(18).getString()},
                    {"max_consecutive_episodes", bq.getColumn(19).getInt()},
                };
                if (!bq.getColumn(5).isNull()) block_j["end_time"] = bq.getColumn(5).getString();

                SQLite::Statement ccq(db_.get(), R"(
                    SELECT bc.content_type, bc.weight, bc.run_count, bc.season_filter,
                           CASE bc.content_type
                               WHEN 'show'        THEN COALESCE(sw.title,'')
                               WHEN 'movie'       THEN COALESCE(m.title,'')
                               WHEN 'episode'     THEN COALESCE(esw.title,'') ||
                                                       ' S' || PRINTF('%02d',e.season) ||
                                                       'E' || PRINTF('%02d',e.episode)
                               WHEN 'playlist'    THEN COALESCE(pl.title,'')
                               WHEN 'filler_list' THEN COALESCE(fl.title,'')
                               ELSE ''
                           END AS title,
                           COALESCE(sw.imdb_id,''), COALESCE(sw.tvdb_id,''), COALESCE(sw.tmdb_id,''),
                           COALESCE(m.imdb_id,''),  COALESCE(m.tmdb_id,''),
                           e.season, e.episode,
                           COALESCE(esw.imdb_id,''), COALESCE(esw.tvdb_id,''), COALESCE(esw.tmdb_id,''),
                           sw.year, m.year
                    FROM block_content bc
                    LEFT JOIN show        sw  ON bc.content_type = 'show'        AND bc.content_id = sw.show_id
                    LEFT JOIN movie       m   ON bc.content_type = 'movie'       AND bc.content_id = m.movie_id
                    LEFT JOIN episode     e   ON bc.content_type = 'episode'     AND bc.content_id = e.episode_id
                    LEFT JOIN show        esw ON bc.content_type = 'episode'     AND e.show_id = esw.show_id
                    LEFT JOIN playlist    pl  ON bc.content_type = 'playlist'    AND bc.content_id = pl.playlist_id
                    LEFT JOIN filler_list fl  ON bc.content_type = 'filler_list' AND bc.content_id = fl.filler_list_id
                    WHERE bc.block_id = ? ORDER BY bc.position
                )");
                ccq.bind(1, bid);
                json content = json::array();
                while (ccq.executeStep()) {
                    std::string ct = ccq.getColumn(0).getString();
                    json item = {
                        {"content_type", ct},
                        {"weight",       ccq.getColumn(1).getInt()},
                        {"run_count",    ccq.getColumn(2).getInt()},
                        {"title",        ccq.getColumn(4).getString()},
                    };
                    if (!ccq.getColumn(3).isNull()) item["season_filter"] = ccq.getColumn(3).getInt();
                    if (ct == "show"  && !ccq.getColumn(15).isNull()) item["year"] = ccq.getColumn(15).getInt();
                    if (ct == "movie" && !ccq.getColumn(16).isNull()) item["year"] = ccq.getColumn(16).getInt();
                    if (deep) {
                        if (ct == "show") {
                            item["imdb_id"] = ccq.getColumn(5).getString();
                            item["tvdb_id"] = ccq.getColumn(6).getString();
                            item["tmdb_id"] = ccq.getColumn(7).getString();
                        } else if (ct == "movie") {
                            item["imdb_id"] = ccq.getColumn(8).getString();
                            item["tmdb_id"] = ccq.getColumn(9).getString();
                        } else if (ct == "episode") {
                            if (!ccq.getColumn(10).isNull()) item["season"]  = ccq.getColumn(10).getInt();
                            if (!ccq.getColumn(11).isNull()) item["episode"] = ccq.getColumn(11).getInt();
                            item["show_imdb_id"] = ccq.getColumn(12).getString();
                            item["show_tvdb_id"] = ccq.getColumn(13).getString();
                            item["show_tmdb_id"] = ccq.getColumn(14).getString();
                        }
                    }
                    content.push_back(item);
                }
                block_j["content"] = content;

                SQLite::Statement bfq(db_.get(), R"(
                    SELECT fl.title, bfe.advancement, bfe.weight
                    FROM block_filler_entry bfe
                    JOIN filler_list fl ON fl.filler_list_id = bfe.filler_list_id
                    WHERE bfe.block_id = ? ORDER BY bfe.position
                )");
                bfq.bind(1, bid);
                json bfiller = json::array();
                while (bfq.executeStep()) {
                    bfiller.push_back({
                        {"title",       bfq.getColumn(0).getString()},
                        {"advancement", bfq.getColumn(1).getString()},
                        {"weight",      bfq.getColumn(2).getInt()},
                    });
                }
                block_j["filler_entries"] = bfiller;
                blocks.push_back(block_j);
            }

            ok(res, json{
                {"kairos_export", 1},
                {"depth",         depth},
                {"channel",       channel_j},
                {"blocks",        blocks},
            }.dump(2));
        } catch (const std::exception& e) {
            logErr("GET /api/channels/:id/export", e); err(res, 500, e.what());
        }
    });

    svr_.Post("/api/channels/import", [this](const Req& req, Res& res) {
        try {
            auto body  = json::parse(req.body);
            auto ch    = body.at("channel");
            bool deep  = (body.value("depth", "shallow") == "deep");

            std::string channel_id = generateId();
            {
                SQLite::Statement s(db_.get(), R"(
                    INSERT INTO channel (channel_id, name, number, timezone, advance_mode,
                                        default_filler_selection)
                    VALUES (?,?,?,?,?,?)
                )");
                std::string imp_tz = ch.value("timezone", "UTC");
                if (!isValidTimezone(imp_tz)) imp_tz = "UTC";
                s.bind(1, channel_id);
                s.bind(2, ch.value("name", "Imported Channel"));
                s.bind(3, ch.value("number", 1));
                s.bind(4, imp_tz);
                s.bind(5, ch.value("advance_mode", "scheduled"));
                s.bind(6, ch.value("default_filler_selection", "round_robin"));
                s.exec();
            }
            if (ch.contains("seed") && !ch["seed"].is_null()) {
                SQLite::Statement s(db_.get(), "UPDATE channel SET seed = ? WHERE channel_id = ?");
                s.bind(1, ch["seed"].get<int>()); s.bind(2, channel_id); s.exec();
            }

            // Channel filler entries — resolve by title
            int cfpos = 0;
            for (auto& fe : ch.value("default_filler_entries", json::array())) {
                std::string ftitle = fe.value("title", "");
                SQLite::Statement fq(db_.get(), "SELECT filler_list_id FROM filler_list WHERE title = ? LIMIT 1");
                fq.bind(1, ftitle);
                if (!fq.executeStep()) { cfpos++; continue; }
                std::string fid = fq.getColumn(0).getString();
                SQLite::Statement ins(db_.get(), R"(
                    INSERT OR IGNORE INTO channel_filler_entry
                        (channel_id, filler_list_id, advancement, weight, position)
                    VALUES (?,?,?,?,?)
                )");
                ins.bind(1, channel_id); ins.bind(2, fid);
                ins.bind(3, fe.value("advancement", "sized")); ins.bind(4, fe.value("weight", 1));
                ins.bind(5, cfpos++); ins.exec();
            }

            json unresolved = json::array();

            for (auto& blk : body.value("blocks", json::array())) {
                std::string block_id = generateId();
                std::string end_time = blk.value("end_time", "");
                SQLite::Statement s(db_.get(), R"(
                    INSERT INTO block (block_id, channel_id, name, block_type, day_mask,
                                       start_time, end_time, program_count, priority,
                                       max_content_rating, advancement, cursor_scope,
                                       late_start_mins, align_to_mins, inter_filler,
                                       early_start_secs, filler_selection, smart_pct,
                                       start_scope, no_history_behavior,
                                       max_consecutive_episodes)
                    VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
                )");
                s.bind(1, block_id); s.bind(2, channel_id);
                s.bind(3, blk.value("name", ""));
                s.bind(4, blk.value("block_type", "episode"));
                s.bind(5, blk.value("day_mask", 127));
                s.bind(6, blk.value("start_time", "00:00"));
                if (end_time.empty()) s.bind(7); else s.bind(7, end_time);
                s.bind(8,  blk.value("program_count",      0));
                s.bind(9,  blk.value("priority",           0));
                s.bind(10, blk.value("max_content_rating", ""));
                s.bind(11, blk.value("advancement",        "sequential"));
                s.bind(12, blk.value("cursor_scope",       "block"));
                s.bind(13, blk.value("late_start_mins",    0));
                s.bind(14, blk.value("align_to_mins",      0));
                s.bind(15, blk.value("inter_filler", false) ? 1 : 0);
                s.bind(16, blk.value("early_start_secs",         0));
                s.bind(17, blk.value("filler_selection",   "round_robin"));
                s.bind(18, blk.value("smart_pct",          30));
                s.bind(19, blk.value("start_scope",        "block"));
                s.bind(20, blk.value("no_history_behavior", "normal"));
                s.bind(21, blk.value("max_consecutive_episodes", 0));
                s.exec();

                // Content items
                int pos = 0;
                std::string block_name = blk.value("name", "");
                for (auto& item : blk.value("content", json::array())) {
                    std::string ct    = item.value("content_type", "");
                    std::string title = item.value("title", "");
                    std::string content_id;

                    auto tryQuery = [&](const char* sql, const std::string& val) {
                        if (!content_id.empty() || val.empty()) return;
                        SQLite::Statement q(db_.get(), sql);
                        q.bind(1, val);
                        if (q.executeStep()) content_id = q.getColumn(0).getString();
                    };

                    if (ct == "show") {
                        if (deep) {
                            tryQuery("SELECT show_id FROM show WHERE imdb_id = ? AND imdb_id != '' LIMIT 1", item.value("imdb_id",""));
                            tryQuery("SELECT show_id FROM show WHERE tvdb_id = ? AND tvdb_id != '' LIMIT 1", item.value("tvdb_id",""));
                            tryQuery("SELECT show_id FROM show WHERE tmdb_id = ? AND tmdb_id != '' LIMIT 1", item.value("tmdb_id",""));
                        }
                        if (!content_id.empty()); // skip title lookups if already resolved
                        else if (item.contains("year") && !item["year"].is_null()) {
                            SQLite::Statement q(db_.get(), "SELECT show_id FROM show WHERE title = ? AND year = ? LIMIT 1");
                            q.bind(1, title); q.bind(2, item["year"].get<int>());
                            if (q.executeStep()) content_id = q.getColumn(0).getString();
                        }
                        tryQuery("SELECT show_id FROM show WHERE title = ? LIMIT 1", title);
                    } else if (ct == "movie") {
                        if (deep) {
                            tryQuery("SELECT movie_id FROM movie WHERE imdb_id = ? AND imdb_id != '' LIMIT 1", item.value("imdb_id",""));
                            tryQuery("SELECT movie_id FROM movie WHERE tmdb_id = ? AND tmdb_id != '' LIMIT 1", item.value("tmdb_id",""));
                        }
                        if (!content_id.empty()); // skip title lookups if already resolved
                        else if (item.contains("year") && !item["year"].is_null()) {
                            SQLite::Statement q(db_.get(), "SELECT movie_id FROM movie WHERE title = ? AND year = ? LIMIT 1");
                            q.bind(1, title); q.bind(2, item["year"].get<int>());
                            if (q.executeStep()) content_id = q.getColumn(0).getString();
                        }
                        tryQuery("SELECT movie_id FROM movie WHERE title = ? LIMIT 1", title);
                    } else if (ct == "episode" && deep) {
                        std::string show_id;
                        auto tryShow = [&](const char* col, const std::string& val) {
                            if (!show_id.empty() || val.empty()) return;
                            SQLite::Statement q(db_.get(),
                                std::string("SELECT show_id FROM show WHERE ") + col + " = ? AND " + col + " != '' LIMIT 1");
                            q.bind(1, val);
                            if (q.executeStep()) show_id = q.getColumn(0).getString();
                        };
                        tryShow("imdb_id", item.value("show_imdb_id",""));
                        tryShow("tvdb_id", item.value("show_tvdb_id",""));
                        tryShow("tmdb_id", item.value("show_tmdb_id",""));
                        if (!show_id.empty()) {
                            SQLite::Statement q(db_.get(),
                                "SELECT episode_id FROM episode WHERE show_id=? AND season=? AND episode=? LIMIT 1");
                            q.bind(1, show_id);
                            q.bind(2, item.value("season",  0));
                            q.bind(3, item.value("episode", 0));
                            if (q.executeStep()) content_id = q.getColumn(0).getString();
                        }
                    } else if (ct == "playlist") {
                        tryQuery("SELECT playlist_id FROM playlist WHERE title = ? LIMIT 1", title);
                    } else if (ct == "filler_list") {
                        tryQuery("SELECT filler_list_id FROM filler_list WHERE title = ? LIMIT 1", title);
                    }

                    if (content_id.empty()) {
                        unresolved.push_back({
                            {"block_name",   block_name},
                            {"content_type", ct},
                            {"title",        title},
                            {"reason",       "no match found"},
                        });
                        pos++; continue;
                    }

                    SQLite::Statement ins(db_.get(), R"(
                        INSERT OR IGNORE INTO block_content
                            (block_id, content_type, content_id, position, weight, run_count)
                        VALUES (?,?,?,?,?,?)
                    )");
                    ins.bind(1, block_id); ins.bind(2, ct); ins.bind(3, content_id);
                    ins.bind(4, pos);
                    ins.bind(5, item.value("weight",    1));
                    ins.bind(6, item.value("run_count", 1));
                    ins.exec();

                    if (item.contains("season_filter") && !item["season_filter"].is_null()) {
                        SQLite::Statement upd(db_.get(), R"(
                            UPDATE block_content SET season_filter = ?
                            WHERE block_id = ? AND content_type = ? AND content_id = ?
                        )");
                        upd.bind(1, item["season_filter"].get<int>());
                        upd.bind(2, block_id); upd.bind(3, ct); upd.bind(4, content_id);
                        upd.exec();
                    }
                    pos++;
                }

                // Block filler entries
                for (auto& fe : blk.value("filler_entries", json::array())) {
                    std::string ftitle = fe.value("title", "");
                    SQLite::Statement fq(db_.get(), "SELECT filler_list_id FROM filler_list WHERE title = ? LIMIT 1");
                    fq.bind(1, ftitle);
                    if (!fq.executeStep()) continue;
                    std::string fid = fq.getColumn(0).getString();
                    SQLite::Statement pq(db_.get(),
                        "SELECT COALESCE(MAX(position),-1)+1 FROM block_filler_entry WHERE block_id = ?");
                    pq.bind(1, block_id); pq.executeStep();
                    SQLite::Statement ins(db_.get(), R"(
                        INSERT OR IGNORE INTO block_filler_entry
                            (block_id, filler_list_id, advancement, weight, position)
                        VALUES (?,?,?,?,?)
                    )");
                    ins.bind(1, block_id); ins.bind(2, fid);
                    ins.bind(3, fe.value("advancement", "sized")); ins.bind(4, fe.value("weight", 1));
                    ins.bind(5, pq.getColumn(0).getInt()); ins.exec();
                }
            }

            res.status = 201;
            ok(res, json{{"channel_id", channel_id}, {"unresolved", unresolved}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    // ── Channel filler entry CRUD ─────────────────────────────────────────────

    svr_.Post("/api/channels/:id/filler", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            std::string filler_list_id = b.value("filler_list_id", "");
            std::string advancement    = b.value("advancement",    "sequential");
            int         weight         = b.value("weight",         1);
            if (filler_list_id.empty()) { err(res, 400, "filler_list_id required"); return; }

            int position = 0;
            {
                SQLite::Statement pq(db_.get(),
                    "SELECT COALESCE(MAX(position), -1) + 1 FROM channel_filler_entry WHERE channel_id = ?");
                pq.bind(1, channel_id);
                if (pq.executeStep()) position = pq.getColumn(0).getInt();
            }
            std::string title;
            {
                SQLite::Statement tq(db_.get(),
                    "SELECT title FROM filler_list WHERE filler_list_id = ?");
                tq.bind(1, filler_list_id);
                if (tq.executeStep()) title = tq.getColumn(0).getString();
            }
            SQLite::Statement s(db_.get(), R"(
                INSERT INTO channel_filler_entry
                    (channel_id, filler_list_id, advancement, weight, position)
                VALUES (?,?,?,?,?)
            )");
            s.bind(1, channel_id); s.bind(2, filler_list_id); s.bind(3, advancement);
            s.bind(4, weight);     s.bind(5, position);
            s.exec();
            int64_t new_id = db_.get().getLastInsertRowid();
            res.status = 201;
            ok(res, json{{"id", new_id}, {"filler_list_id", filler_list_id},
                         {"title", title}, {"advancement", advancement},
                         {"weight", weight}, {"position", position}}.dump());
        } catch (const SQLite::Exception& e) { err(res, 409, e.what()); }
          catch (const std::exception& e)    { err(res, 400, e.what()); }
    });

    svr_.Patch("/api/channels/:id/filler/:eid", [this](const Req& req, Res& res) {
        auto eid = std::stoi(req.path_params.at("eid"));
        try {
            auto b = json::parse(req.body);
            if (b.contains("advancement")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE channel_filler_entry SET advancement = ? WHERE id = ?");
                s.bind(1, b["advancement"].get<std::string>()); s.bind(2, eid); s.exec();
            }
            if (b.contains("weight")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE channel_filler_entry SET weight = ? WHERE id = ?");
                s.bind(1, b["weight"].get<int>()); s.bind(2, eid); s.exec();
            }
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Delete("/api/channels/:id/filler/:eid", [this](const Req& req, Res& res) {
        auto eid = std::stoi(req.path_params.at("eid"));
        SQLite::Statement s(db_.get(), "DELETE FROM channel_filler_entry WHERE id = ?");
        s.bind(1, eid); s.exec();
        ok(res, json{{"deleted", eid}}.dump());
    });
}

// ---------------------------------------------------------------------------
// Blocks — schedule blocks and their content for each channel
// ---------------------------------------------------------------------------

void Router::registerBlockRoutes() {

    // List all blocks for a channel (with content items + title joins)
    svr_.Get("/api/channels/:id/blocks", [this](const Req& req, Res& res) {
      try {
        auto channel_id = req.path_params.at("id");

        SQLite::Statement q(db_.get(), R"(
            SELECT block_id, block_type, day_mask, start_time, end_time,
                   program_count, priority, max_content_rating, advancement, cursor_scope,
                   late_start_mins, align_to_mins, inter_filler, early_start_secs,
                   filler_selection, smart_pct, start_scope, no_history_behavior,
                   max_consecutive_episodes, name,
                   intro_content_type, intro_content_id,
                   outro_content_type, outro_content_id,
                   interstitial_content_type, interstitial_content_id,
                   interstitial_every_n
            FROM block WHERE channel_id = ?
            ORDER BY start_time, priority DESC
        )");
        q.bind(1, channel_id);

        json result = json::array();
        while (q.executeStep()) {
            std::string bid = q.getColumn(0).getString();
            json block = {
                {"block_id",                   bid},
                {"channel_id",                 channel_id},
                {"block_type",                 q.getColumn(1).getString()},
                {"day_mask",                   q.getColumn(2).getInt()},
                {"start_time",                 q.getColumn(3).getString()},
                {"program_count",              q.getColumn(5).getInt()},
                {"priority",                   q.getColumn(6).getInt()},
                {"max_content_rating",         q.getColumn(7).getString()},
                {"advancement",                q.getColumn(8).getString()},
                {"cursor_scope",               q.getColumn(9).getString()},
                {"late_start_mins",            q.getColumn(10).getInt()},
                {"align_to_mins",              q.getColumn(11).getInt()},
                {"inter_filler",               q.getColumn(12).getInt() != 0},
                {"early_start_secs",           q.getColumn(13).getInt()},
                {"filler_selection",           q.getColumn(14).getString()},
                {"smart_pct",                  q.getColumn(15).getInt()},
                {"start_scope",                q.getColumn(16).getString()},
                {"no_history_behavior",        q.getColumn(17).getString()},
                {"max_consecutive_episodes",   q.getColumn(18).getInt()},
                {"name",                       q.getColumn(19).getString()},
                {"intro_content_type",         q.getColumn(20).getString()},
                {"intro_content_id",           q.getColumn(21).getString()},
                {"outro_content_type",         q.getColumn(22).getString()},
                {"outro_content_id",           q.getColumn(23).getString()},
                {"interstitial_content_type",  q.getColumn(24).getString()},
                {"interstitial_content_id",    q.getColumn(25).getString()},
                {"interstitial_every_n",       q.getColumn(26).getInt()},
            };
            if (!q.getColumn(4).isNull()) block["end_time"] = q.getColumn(4).getString();

            // Content items — multi-type title join
            SQLite::Statement cq(db_.get(), R"(
                SELECT bc.id, bc.content_type, bc.content_id, bc.position, bc.season_filter,
                       bc.weight, bc.run_count, bc.include_specials, bc.episode_order,
                       CASE bc.content_type
                           WHEN 'show'        THEN COALESCE(sw.title,'') ||
                                                   CASE WHEN bc.season_filter IS NOT NULL
                                                        THEN ' — Season ' || bc.season_filter
                                                        ELSE '' END
                           WHEN 'episode'     THEN COALESCE(es.title,'') ||
                                                   ' S' || PRINTF('%02d',e.season) ||
                                                   'E' || PRINTF('%02d',e.episode) ||
                                                   ' — ' || COALESCE(e.title,'')
                           WHEN 'movie'       THEN COALESCE(m.title,'')
                           WHEN 'playlist'    THEN COALESCE(pl.title,'')
                           WHEN 'filler_list' THEN COALESCE(fl.title,'')
                           ELSE ''
                       END AS title
                FROM block_content bc
                LEFT JOIN show        sw ON bc.content_type = 'show'        AND bc.content_id = sw.show_id
                LEFT JOIN episode      e ON bc.content_type = 'episode'     AND bc.content_id = e.episode_id
                LEFT JOIN show        es ON bc.content_type = 'episode'     AND e.show_id = es.show_id
                LEFT JOIN movie        m ON bc.content_type = 'movie'       AND bc.content_id = m.movie_id
                LEFT JOIN playlist    pl ON bc.content_type = 'playlist'    AND bc.content_id = pl.playlist_id
                LEFT JOIN filler_list fl ON bc.content_type = 'filler_list' AND bc.content_id = fl.filler_list_id
                WHERE bc.block_id = ? ORDER BY bc.position
            )");
            cq.bind(1, bid);
            json content = json::array();
            while (cq.executeStep()) {
                json item = {
                    {"id",               cq.getColumn(0).getInt()},
                    {"content_type",     cq.getColumn(1).getString()},
                    {"content_id",       cq.getColumn(2).getString()},
                    {"position",         cq.getColumn(3).getInt()},
                    {"weight",           cq.getColumn(5).getInt()},
                    {"run_count",        cq.getColumn(6).getInt()},
                    {"include_specials", cq.getColumn(7).getInt() != 0},
                    {"episode_order",    cq.getColumn(8).getString()},
                    {"title",            cq.getColumn(9).getString()},
                };
                if (!cq.getColumn(4).isNull()) item["season_filter"] = cq.getColumn(4).getInt();
                content.push_back(item);
            }
            block["content"] = content;

            // Filler entries for this block
            SQLite::Statement fq(db_.get(), R"(
                SELECT bfe.id, bfe.filler_list_id, fl.title,
                       bfe.advancement, bfe.weight, bfe.position
                FROM block_filler_entry bfe
                JOIN filler_list fl ON fl.filler_list_id = bfe.filler_list_id
                WHERE bfe.block_id = ? ORDER BY bfe.position
            )");
            fq.bind(1, bid);
            json filler_entries = json::array();
            while (fq.executeStep()) {
                filler_entries.push_back({
                    {"id",             fq.getColumn(0).getInt()},
                    {"filler_list_id", fq.getColumn(1).getString()},
                    {"title",          fq.getColumn(2).getString()},
                    {"advancement",    fq.getColumn(3).getString()},
                    {"weight",         fq.getColumn(4).getInt()},
                    {"position",       fq.getColumn(5).getInt()},
                });
            }
            block["filler_entries"] = filler_entries;

            result.push_back(block);
        }
        ok(res, result.dump());
      } catch (const std::exception& e) {
        logErr("GET /api/channels/:id/blocks", e); err(res, 500, e.what());
      }
    });

    // Create block
    svr_.Post("/api/channels/:id/blocks", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            std::string block_id         = generateId();
            std::string block_name       = b.value("name",               "");
            std::string block_type       = b.value("block_type",         "episode");
            int         day_mask         = b.value("day_mask",           127);
            std::string start_time       = b.value("start_time",         "00:00");
            std::string end_time         = b.value("end_time",           "");
            int         program_count    = b.value("program_count",      0);
            int         priority         = b.value("priority",           0);
            std::string max_rating       = b.value("max_content_rating", "");
            std::string advancement      = b.value("advancement",        "sequential");
            std::string cursor_scope     = b.value("cursor_scope",       "block");
            int         late_start_mins  = b.value("late_start_mins",    0);
            int         align_to_mins    = b.value("align_to_mins",      0);
            int         inter_filler     = b.value("inter_filler",       false) ? 1 : 0;
            int         early_start_secs = b.value("early_start_secs",   0);
            std::string filler_selection = b.value("filler_selection",   "round_robin");
            int         smart_pct                = b.value("smart_pct",                30);
            std::string start_scope             = b.value("start_scope",             "block");
            std::string no_history_behavior     = b.value("no_history_behavior",     "normal");
            int         max_consecutive_episodes = b.value("max_consecutive_episodes", 0);
            std::string intro_ct   = b.value("intro_content_type",          "");
            std::string intro_cid  = b.value("intro_content_id",            "");
            std::string outro_ct   = b.value("outro_content_type",          "");
            std::string outro_cid  = b.value("outro_content_id",            "");
            std::string inter_ct   = b.value("interstitial_content_type",   "");
            std::string inter_cid  = b.value("interstitial_content_id",     "");
            int         inter_n    = b.value("interstitial_every_n",        1);

            SQLite::Statement s(db_.get(), R"(
                INSERT INTO block (block_id, channel_id, name, block_type, day_mask,
                                   start_time, end_time, program_count, priority,
                                   max_content_rating, advancement, cursor_scope,
                                   late_start_mins, align_to_mins, inter_filler,
                                   early_start_secs, filler_selection, smart_pct,
                                   start_scope, no_history_behavior,
                                   max_consecutive_episodes,
                                   intro_content_type, intro_content_id,
                                   outro_content_type, outro_content_id,
                                   interstitial_content_type, interstitial_content_id,
                                   interstitial_every_n)
                VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            )");
            s.bind(1, block_id);       s.bind(2, channel_id);    s.bind(3, block_name);
            s.bind(4, block_type);     s.bind(5, day_mask);       s.bind(6, start_time);
            if (end_time.empty()) s.bind(7); else s.bind(7, end_time);
            s.bind(8, program_count);  s.bind(9, priority);       s.bind(10, max_rating);
            s.bind(11, advancement);   s.bind(12, cursor_scope);  s.bind(13, late_start_mins);
            s.bind(14, align_to_mins); s.bind(15, inter_filler);  s.bind(16, early_start_secs);
            s.bind(17, filler_selection); s.bind(18, smart_pct);  s.bind(19, start_scope);
            s.bind(20, no_history_behavior); s.bind(21, max_consecutive_episodes);
            s.bind(22, intro_ct);  s.bind(23, intro_cid);
            s.bind(24, outro_ct);  s.bind(25, outro_cid);
            s.bind(26, inter_ct);  s.bind(27, inter_cid);  s.bind(28, inter_n);
            s.exec();

            clearScheduleCache(channel_id);
            res.status = 201;
            ok(res, json{{"block_id", block_id}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    // Update block properties
    svr_.Patch("/api/channels/:id/blocks/:bid", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto bid        = req.path_params.at("bid");
        try {
            auto b = json::parse(req.body);
            auto upd = [&](const char* col, const std::string& val) {
                SQLite::Statement s(db_.get(),
                    std::string("UPDATE block SET ") + col + " = ? WHERE block_id = ?");
                s.bind(1, val); s.bind(2, bid); s.exec();
            };
            auto updI = [&](const char* col, int val) {
                SQLite::Statement s(db_.get(),
                    std::string("UPDATE block SET ") + col + " = ? WHERE block_id = ?");
                s.bind(1, val); s.bind(2, bid); s.exec();
            };
            auto updNull = [&](const char* col) {
                SQLite::Statement s(db_.get(),
                    std::string("UPDATE block SET ") + col + " = NULL WHERE block_id = ?");
                s.bind(1, bid); s.exec();
            };
            if (b.contains("name"))                upd("name",               b["name"]);
            if (b.contains("block_type"))         upd("block_type",         b["block_type"]);
            if (b.contains("day_mask"))            updI("day_mask",          b["day_mask"]);
            if (b.contains("start_time"))          upd("start_time",         b["start_time"]);
            if (b.contains("priority"))            updI("priority",          b["priority"]);
            if (b.contains("program_count"))       updI("program_count",     b["program_count"]);
            if (b.contains("late_start_mins"))     updI("late_start_mins",   b["late_start_mins"]);
            if (b.contains("advancement"))         upd("advancement",        b["advancement"]);
            if (b.contains("cursor_scope"))        upd("cursor_scope",       b["cursor_scope"]);
            if (b.contains("max_content_rating"))  upd("max_content_rating", b["max_content_rating"]);
            if (b.contains("align_to_mins"))       updI("align_to_mins",     b["align_to_mins"]);
            if (b.contains("inter_filler"))        updI("inter_filler",      b["inter_filler"].is_boolean() ? (b["inter_filler"].get<bool>() ? 1 : 0) : b["inter_filler"].get<int>());
            if (b.contains("early_start_secs"))    updI("early_start_secs",  b["early_start_secs"]);
            if (b.contains("filler_selection"))    upd("filler_selection",   b["filler_selection"]);
            if (b.contains("smart_pct"))                   updI("smart_pct",                   b["smart_pct"]);
            if (b.contains("start_scope"))                 upd("start_scope",                  b["start_scope"]);
            if (b.contains("no_history_behavior"))         upd("no_history_behavior",          b["no_history_behavior"]);
            if (b.contains("max_consecutive_episodes"))    updI("max_consecutive_episodes",    b["max_consecutive_episodes"]);
            if (b.contains("intro_content_type"))          upd("intro_content_type",           b["intro_content_type"]);
            if (b.contains("intro_content_id"))            upd("intro_content_id",             b["intro_content_id"]);
            if (b.contains("outro_content_type"))          upd("outro_content_type",           b["outro_content_type"]);
            if (b.contains("outro_content_id"))            upd("outro_content_id",             b["outro_content_id"]);
            if (b.contains("interstitial_content_type"))   upd("interstitial_content_type",    b["interstitial_content_type"]);
            if (b.contains("interstitial_content_id"))     upd("interstitial_content_id",      b["interstitial_content_id"]);
            if (b.contains("interstitial_every_n"))        updI("interstitial_every_n",        b["interstitial_every_n"]);
            if (b.contains("end_time")) {
                if (b["end_time"].is_null() || b["end_time"].get<std::string>().empty())
                    updNull("end_time");
                else
                    upd("end_time", b["end_time"]);
            }
            clearScheduleCache(channel_id);
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    // Delete block (cascade deletes content via FK)
    svr_.Delete("/api/channels/:id/blocks/:bid", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto bid        = req.path_params.at("bid");
        try {
            SQLite::Statement s(db_.get(), "DELETE FROM block WHERE block_id = ?");
            s.bind(1, bid); s.exec();
            clearScheduleCache(channel_id);
            ok(res, json{{"deleted", bid}}.dump());
        } catch (const std::exception& e) { err(res, 500, e.what()); }
    });

    // Add content to block
    svr_.Post("/api/channels/:id/blocks/:bid/content", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto bid        = req.path_params.at("bid");
        try {
            auto b = json::parse(req.body);
            std::string content_type = b.value("content_type", "show");
            std::string content_id   = b.value("content_id",   "");
            int         weight       = b.value("weight",    1);
            int         run_count    = b.value("run_count", 1);

            int position = 0;
            {
                SQLite::Statement pq(db_.get(),
                    "SELECT COALESCE(MAX(position), -1) + 1 FROM block_content WHERE block_id = ?");
                pq.bind(1, bid);
                if (pq.executeStep()) position = pq.getColumn(0).getInt();
            }

            bool include_specials = b.value("include_specials", false);
            std::string episode_order = b.value("episode_order", std::string("season"));

            SQLite::Statement s(db_.get(), R"(
                INSERT INTO block_content
                    (block_id, content_type, content_id, position, season_filter, weight, run_count,
                     include_specials, episode_order)
                VALUES (?,?,?,?,?,?,?,?,?)
            )");
            s.bind(1, bid); s.bind(2, content_type); s.bind(3, content_id); s.bind(4, position);
            if (b.contains("season_filter") && !b["season_filter"].is_null())
                s.bind(5, b["season_filter"].get<int>());
            else
                s.bind(5);  // NULL
            s.bind(6, weight); s.bind(7, run_count);
            s.bind(8, include_specials ? 1 : 0); s.bind(9, episode_order);
            s.exec();

            clearScheduleCache(channel_id);
            res.status = 201;
            ok(res, json{{"id", db_.get().getLastInsertRowid()}, {"position", position}}.dump());
        } catch (const SQLite::Exception& e) { err(res, 409, e.what()); }
          catch (const std::exception& e)    { err(res, 400, e.what()); }
    });

    // Update content item (season_filter or reorder)
    svr_.Patch("/api/channels/:id/blocks/:bid/content/:cid", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto cid        = req.path_params.at("cid");
        try {
            auto b = json::parse(req.body);
            if (b.contains("season_filter")) {
                if (b["season_filter"].is_null()) {
                    SQLite::Statement s(db_.get(),
                        "UPDATE block_content SET season_filter = NULL WHERE id = ?");
                    s.bind(1, std::stoi(cid)); s.exec();
                } else {
                    SQLite::Statement s(db_.get(),
                        "UPDATE block_content SET season_filter = ? WHERE id = ?");
                    s.bind(1, b["season_filter"].get<int>()); s.bind(2, std::stoi(cid)); s.exec();
                }
            }
            if (b.contains("position")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE block_content SET position = ? WHERE id = ?");
                s.bind(1, b["position"].get<int>()); s.bind(2, std::stoi(cid)); s.exec();
            }
            if (b.contains("weight")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE block_content SET weight = ? WHERE id = ?");
                s.bind(1, b["weight"].get<int>()); s.bind(2, std::stoi(cid)); s.exec();
            }
            if (b.contains("run_count")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE block_content SET run_count = ? WHERE id = ?");
                s.bind(1, b["run_count"].get<int>()); s.bind(2, std::stoi(cid)); s.exec();
            }
            if (b.contains("include_specials")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE block_content SET include_specials = ? WHERE id = ?");
                s.bind(1, b["include_specials"].get<bool>() ? 1 : 0); s.bind(2, std::stoi(cid)); s.exec();
            }
            if (b.contains("episode_order")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE block_content SET episode_order = ? WHERE id = ?");
                s.bind(1, b["episode_order"].get<std::string>()); s.bind(2, std::stoi(cid)); s.exec();
            }
            clearScheduleCache(channel_id);
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    // Remove content item
    svr_.Delete("/api/channels/:id/blocks/:bid/content/:cid", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto cid        = req.path_params.at("cid");
        SQLite::Statement s(db_.get(), "DELETE FROM block_content WHERE id = ?");
        s.bind(1, std::stoi(cid)); s.exec();
        clearScheduleCache(channel_id);
        ok(res, json{{"deleted", std::stoi(cid)}}.dump());
    });

    // Reset cursor for a show content item
    svr_.Delete("/api/channels/:id/blocks/:bid/content/:cid/cursor", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto block_id   = req.path_params.at("bid");
        int  cid        = std::stoi(req.path_params.at("cid"));

        std::string content_type, content_id;
        {
            SQLite::Statement q(db_.get(),
                "SELECT content_type, content_id FROM block_content WHERE id = ?");
            q.bind(1, cid);
            if (!q.executeStep()) { err(res, 404, "content item not found"); return; }
            content_type = q.getColumn(0).getString();
            content_id   = q.getColumn(1).getString();
        }
        if (content_type != "show") { err(res, 400, "cursor reset only applies to show content"); return; }

        std::string advancement, cursor_scope;
        {
            SQLite::Statement q(db_.get(),
                "SELECT advancement, cursor_scope FROM block WHERE block_id = ?");
            q.bind(1, block_id);
            if (!q.executeStep()) { err(res, 404, "block not found"); return; }
            advancement  = q.getColumn(0).getString();
            cursor_scope = q.getColumn(1).getString();
        }

        if (advancement == "rerun_shuffle" || advancement == "rerun_smart") {
            // Rerun episode cursors are block-scoped show_rerun rows keyed by content_id.
            // Only delete this show's episode position; leave block_state (show selection) intact.
            SQLite::Statement s(db_.get(), R"(
                DELETE FROM media_cursor
                WHERE content_type='show_rerun' AND content_id=? AND cursor_scope='block' AND scope_id=?
            )");
            s.bind(1, content_id); s.bind(2, block_id); s.exec();
        } else {
            std::string scope_id;
            if      (cursor_scope == "global")  scope_id = "";
            else if (cursor_scope == "channel") scope_id = channel_id;
            else                                scope_id = block_id;

            SQLite::Statement s(db_.get(), R"(
                DELETE FROM media_cursor
                WHERE content_type='show' AND content_id=? AND cursor_scope=? AND scope_id=?
            )");
            s.bind(1, content_id); s.bind(2, cursor_scope); s.bind(3, scope_id); s.exec();
        }

        clearScheduleCache(channel_id);
        ok(res, json{{"ok", true}}.dump());
    });

    // ── Episode groups (multipart markup) ────────────────────────────────────

    // List groups for a show
    svr_.Get("/api/shows/:id/groups", [this](const Req& req, Res& res) {
        auto show_id = req.path_params.at("id");
        SQLite::Statement q(db_.get(),
            "SELECT group_id, name, group_type FROM episode_group WHERE show_id=? ORDER BY name");
        q.bind(1, show_id);
        json arr = json::array();
        while (q.executeStep()) {
            std::string gid = q.getColumn(0).getString();
            json g = {{"group_id", gid}, {"name", q.getColumn(1).getString()},
                      {"group_type", q.getColumn(2).getString()}, {"members", json::array()}};
            SQLite::Statement mq(db_.get(),
                "SELECT egm.id, egm.episode_id, egm.part_num, e.season, e.episode, e.title "
                "FROM episode_group_member egm JOIN episode e ON e.episode_id=egm.episode_id "
                "WHERE egm.group_id=? ORDER BY egm.part_num");
            mq.bind(1, gid);
            while (mq.executeStep())
                g["members"].push_back({{"id", mq.getColumn(0).getInt()},
                    {"episode_id", mq.getColumn(1).getString()},
                    {"part_num",   mq.getColumn(2).getInt()},
                    {"season",     mq.getColumn(3).getInt()},
                    {"episode",    mq.getColumn(4).getInt()},
                    {"title",      mq.getColumn(5).getString()}});
            arr.push_back(g);
        }
        ok(res, arr.dump());
    });

    // Create group
    svr_.Post("/api/shows/:id/groups", [this](const Req& req, Res& res) {
        auto show_id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            std::string group_id   = generateId();
            std::string name       = b.value("name",       "");
            std::string group_type = b.value("group_type", "multipart");
            if (name.empty()) { err(res, 400, "name required"); return; }
            SQLite::Statement s(db_.get(),
                "INSERT INTO episode_group (group_id, show_id, name, group_type) VALUES (?,?,?,?)");
            s.bind(1, group_id); s.bind(2, show_id); s.bind(3, name); s.bind(4, group_type);
            s.exec();
            res.status = 201;
            ok(res, json{{"group_id", group_id}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    // Delete group
    svr_.Delete("/api/shows/:id/groups/:gid", [this](const Req& req, Res& res) {
        auto gid = req.path_params.at("gid");
        SQLite::Statement s(db_.get(), "DELETE FROM episode_group WHERE group_id=?");
        s.bind(1, gid); s.exec();
        ok(res, json{{"deleted", gid}}.dump());
    });

    // Add member to group
    svr_.Post("/api/shows/:id/groups/:gid/members", [this](const Req& req, Res& res) {
        auto gid = req.path_params.at("gid");
        try {
            auto b = json::parse(req.body);
            std::string episode_id = b.value("episode_id", "");
            int         part_num   = b.value("part_num",   1);
            if (episode_id.empty()) { err(res, 400, "episode_id required"); return; }
            SQLite::Statement s(db_.get(),
                "INSERT INTO episode_group_member (group_id, episode_id, part_num) VALUES (?,?,?)");
            s.bind(1, gid); s.bind(2, episode_id); s.bind(3, part_num); s.exec();
            res.status = 201;
            ok(res, json{{"id", db_.get().getLastInsertRowid()}, {"part_num", part_num}}.dump());
        } catch (const SQLite::Exception& e) { err(res, 409, e.what()); }
          catch (const std::exception& e)    { err(res, 400, e.what()); }
    });

    // Remove member
    svr_.Delete("/api/shows/:id/groups/:gid/members/:mid", [this](const Req& req, Res& res) {
        auto mid = req.path_params.at("mid");
        SQLite::Statement s(db_.get(), "DELETE FROM episode_group_member WHERE id=?");
        s.bind(1, std::stoi(mid)); s.exec();
        ok(res, json{{"deleted", std::stoi(mid)}}.dump());
    });

    // ── Block filler entry CRUD ───────────────────────────────────────────────

    svr_.Post("/api/channels/:id/blocks/:bid/filler", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto block_id   = req.path_params.at("bid");
        try {
            auto b = json::parse(req.body);
            std::string filler_list_id = b.value("filler_list_id", "");
            std::string advancement    = b.value("advancement",    "sequential");
            int         weight         = b.value("weight",         1);
            if (filler_list_id.empty()) { err(res, 400, "filler_list_id required"); return; }

            int position = 0;
            {
                SQLite::Statement pq(db_.get(),
                    "SELECT COALESCE(MAX(position), -1) + 1 FROM block_filler_entry WHERE block_id = ?");
                pq.bind(1, block_id);
                if (pq.executeStep()) position = pq.getColumn(0).getInt();
            }
            std::string title;
            {
                SQLite::Statement tq(db_.get(),
                    "SELECT title FROM filler_list WHERE filler_list_id = ?");
                tq.bind(1, filler_list_id);
                if (tq.executeStep()) title = tq.getColumn(0).getString();
            }
            SQLite::Statement s(db_.get(), R"(
                INSERT INTO block_filler_entry
                    (block_id, filler_list_id, advancement, weight, position)
                VALUES (?,?,?,?,?)
            )");
            s.bind(1, block_id); s.bind(2, filler_list_id); s.bind(3, advancement);
            s.bind(4, weight);   s.bind(5, position);
            s.exec();
            int64_t new_id = db_.get().getLastInsertRowid();
            clearScheduleCache(channel_id);
            res.status = 201;
            ok(res, json{{"id", new_id}, {"filler_list_id", filler_list_id},
                         {"title", title}, {"advancement", advancement},
                         {"weight", weight}, {"position", position}}.dump());
        } catch (const SQLite::Exception& e) { err(res, 409, e.what()); }
          catch (const std::exception& e)    { err(res, 400, e.what()); }
    });

    svr_.Patch("/api/channels/:id/blocks/:bid/filler/:eid", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto eid        = std::stoi(req.path_params.at("eid"));
        try {
            auto b = json::parse(req.body);
            if (b.contains("advancement")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE block_filler_entry SET advancement = ? WHERE id = ?");
                s.bind(1, b["advancement"].get<std::string>()); s.bind(2, eid); s.exec();
            }
            if (b.contains("weight")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE block_filler_entry SET weight = ? WHERE id = ?");
                s.bind(1, b["weight"].get<int>()); s.bind(2, eid); s.exec();
            }
            clearScheduleCache(channel_id);
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Delete("/api/channels/:id/blocks/:bid/filler/:eid", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto eid        = std::stoi(req.path_params.at("eid"));
        SQLite::Statement s(db_.get(), "DELETE FROM block_filler_entry WHERE id = ?");
        s.bind(1, eid); s.exec();
        clearScheduleCache(channel_id);
        ok(res, json{{"deleted", eid}}.dump());
    });

    // ── Channel bumpers ───────────────────────────────────────────────────────
    // GET   /api/channels/:id/bumpers          → list all bumpers for channel
    // POST  /api/channels/:id/bumpers          → create bumper
    // PATCH /api/channels/:id/bumpers/:bid     → update bumper fields
    // DELETE /api/channels/:id/bumpers/:bid    → delete bumper

    svr_.Get("/api/channels/:id/bumpers", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        try {
            SQLite::Statement q(db_.get(), R"(
                SELECT id, content_type, content_id, mode, every_n, position
                FROM channel_bumper WHERE channel_id = ? ORDER BY position
            )");
            q.bind(1, channel_id);
            json result = json::array();
            while (q.executeStep()) {
                result.push_back({
                    {"id",           q.getColumn(0).getInt()},
                    {"channel_id",   channel_id},
                    {"content_type", q.getColumn(1).getString()},
                    {"content_id",   q.getColumn(2).getString()},
                    {"mode",         q.getColumn(3).getString()},
                    {"every_n",      q.getColumn(4).getInt()},
                    {"position",     q.getColumn(5).getInt()},
                });
            }
            ok(res, result.dump());
        } catch (const std::exception& e) { logErr("GET /api/channels/:id/bumpers", e); err(res, 500, e.what()); }
    });

    svr_.Post("/api/channels/:id/bumpers", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            std::string ct      = b.value("content_type", "show");
            std::string cid     = b.value("content_id",   "");
            std::string mode    = b.value("mode",         "between");
            int         every_n = b.value("every_n",      3);
            // position = next available slot
            SQLite::Statement pq(db_.get(),
                "SELECT COALESCE(MAX(position)+1,0) FROM channel_bumper WHERE channel_id=?");
            pq.bind(1, channel_id);
            pq.executeStep();
            int position = pq.getColumn(0).getInt();

            SQLite::Statement s(db_.get(), R"(
                INSERT INTO channel_bumper (channel_id, content_type, content_id, mode, every_n, position)
                VALUES (?,?,?,?,?,?)
            )");
            s.bind(1, channel_id); s.bind(2, ct); s.bind(3, cid);
            s.bind(4, mode);       s.bind(5, every_n); s.bind(6, position);
            s.exec();
            int new_id = static_cast<int>(db_.get().getLastInsertRowid());
            clearScheduleCache(channel_id);
            res.status = 201;
            ok(res, json{{"id", new_id}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Patch("/api/channels/:id/bumpers/:bid", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto bumper_id  = std::stoi(req.path_params.at("bid"));
        try {
            auto b = json::parse(req.body);
            auto upd = [&](const char* col, const std::string& val) {
                SQLite::Statement s(db_.get(),
                    std::string("UPDATE channel_bumper SET ") + col + " = ? WHERE id = ?");
                s.bind(1, val); s.bind(2, bumper_id); s.exec();
            };
            auto updI = [&](const char* col, int val) {
                SQLite::Statement s(db_.get(),
                    std::string("UPDATE channel_bumper SET ") + col + " = ? WHERE id = ?");
                s.bind(1, val); s.bind(2, bumper_id); s.exec();
            };
            if (b.contains("content_type")) upd("content_type", b["content_type"]);
            if (b.contains("content_id"))   upd("content_id",   b["content_id"]);
            if (b.contains("mode"))         upd("mode",         b["mode"]);
            if (b.contains("every_n"))      updI("every_n",     b["every_n"]);
            if (b.contains("position"))     updI("position",    b["position"]);
            clearScheduleCache(channel_id);
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Delete("/api/channels/:id/bumpers/:bid", [this](const Req& req, Res& res) {
        auto channel_id = req.path_params.at("id");
        auto bumper_id  = std::stoi(req.path_params.at("bid"));
        try {
            SQLite::Statement s(db_.get(), "DELETE FROM channel_bumper WHERE id = ?");
            s.bind(1, bumper_id); s.exec();
            clearScheduleCache(channel_id);
            ok(res, json{{"deleted", bumper_id}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });
}

// ---------------------------------------------------------------------------
// Content browsing
// ---------------------------------------------------------------------------

void Router::registerContentRoutes() {

    // All configured libraries across all sources — used by the content browser sidebar
    svr_.Get("/api/libraries", [this](const Req&, Res& res) {
        SQLite::Statement q(db_.get(), R"(
            SELECT ml.library_id, ml.source_id, ml.display_name, ml.library_type,
                   ms.display_name AS source_name, ms.source_type
            FROM media_library ml
            JOIN media_source ms ON ms.source_id = ml.source_id
            ORDER BY ms.display_name, ml.display_name
        )");
        json result = json::array();
        while (q.executeStep()) {
            result.push_back({
                {"library_id",   q.getColumn(0).getString()},
                {"source_id",    q.getColumn(1).getString()},
                {"display_name", q.getColumn(2).getString()},
                {"library_type", q.getColumn(3).getString()},
                {"source_name",  q.getColumn(4).getString()},
                {"source_type",  q.getColumn(5).getString()},
            });
        }
        ok(res, result.dump());
    });

    // Distinct metadata values for filter autocomplete
    svr_.Get("/api/metadata/values", [this](const Req& req, Res& res) {
        std::string field, type, library_id;
        if (req.has_param("field"))      field      = req.get_param_value("field");
        if (req.has_param("type"))       type       = req.get_param_value("type");
        if (req.has_param("library_id")) library_id = req.get_param_value("library_id");
        if (field.empty()) { err(res, 400, "field required"); return; }

        try {
            std::set<std::string> seen;
            json values = json::array();
            std::vector<std::string> lib = library_id.empty()
                ? std::vector<std::string>{}
                : std::vector<std::string>{library_id};
            std::string show_lib  = library_id.empty() ? "" :
                " AND EXISTS (SELECT 1 FROM source_mapping sm"
                " WHERE sm.kairos_id = s.show_id AND sm.item_type = 'show'"
                " AND sm.library_id = ?)";
            std::string movie_lib = library_id.empty() ? "" :
                " AND EXISTS (SELECT 1 FROM source_mapping sm"
                " WHERE sm.kairos_id = m.movie_id AND sm.item_type = 'movie'"
                " AND sm.library_id = ?)";

            auto collect = [&](const std::string& sql, const std::vector<std::string>& binds) {
                SQLite::Statement q(db_.get(), sql);
                for (int i = 0; i < (int)binds.size(); ++i) q.bind(i + 1, binds[i]);
                while (q.executeStep()) {
                    auto v = q.getColumn(0).getString();
                    if (!v.empty() && seen.insert(v).second) values.push_back(v);
                }
            };

            if (field == "genre") {
                if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(s.genres) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
                if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(m.genres) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
            } else if (field == "studio") {
                if (type != "movie") collect("SELECT DISTINCT studio FROM show s WHERE studio != ''"         + show_lib  + " ORDER BY studio",         lib);
                if (type != "show")  collect("SELECT DISTINCT studio FROM movie m WHERE studio != ''"        + movie_lib + " ORDER BY studio",         lib);
            } else if (field == "director") {
                if (type != "show")  collect("SELECT DISTINCT director FROM movie m WHERE director != ''"    + movie_lib + " ORDER BY director",        lib);
            } else if (field == "content_rating") {
                if (type != "movie") collect("SELECT DISTINCT content_rating FROM show s WHERE content_rating != ''"  + show_lib  + " ORDER BY content_rating", lib);
                if (type != "show")  collect("SELECT DISTINCT content_rating FROM movie m WHERE content_rating != ''" + movie_lib + " ORDER BY content_rating", lib);
            }
            // Fields absent from DB (actor, network, label, etc.) return empty array

            std::sort(values.begin(), values.end(), [](const json& a, const json& b) {
                return a.get<std::string>() < b.get<std::string>();
            });
            ok(res, json{{"values", values}}.dump());
        } catch (const std::exception& e) { err(res, 500, e.what()); }
    });

    svr_.Get("/api/shows", [this](const Req& req, Res& res) {
        int         limit      = 50, offset = 0;
        std::string library_id, search_q, genre, year_p, rating;
        if (req.has_param("limit"))          limit      = std::stoi(req.get_param_value("limit"));
        if (req.has_param("offset"))         offset     = std::stoi(req.get_param_value("offset"));
        if (req.has_param("library_id"))     library_id = req.get_param_value("library_id");
        if (req.has_param("q"))              search_q   = req.get_param_value("q");
        if (req.has_param("genre"))          genre      = req.get_param_value("genre");
        if (req.has_param("year"))           year_p     = req.get_param_value("year");
        if (req.has_param("content_rating")) rating     = req.get_param_value("content_rating");

        // Build extra WHERE conditions (AND-prefixed; works with "WHERE 1=1" base)
        std::string extras;
        std::vector<std::string> extra_vals;
        if (!search_q.empty()) { extras += " AND s.title LIKE '%' || ? || '%'"; extra_vals.push_back(search_q); }
        if (!genre.empty())    appendJsonInClause("s", "genres",       genre,  extras, extra_vals);
        if (!year_p.empty())   { extras += " AND s.year = CAST(? AS INTEGER)"; extra_vals.push_back(year_p); }
        if (!rating.empty())   appendInClause("s.content_rating",      rating, extras, extra_vals);

        auto bindExtras = [&](SQLite::Statement& q, int& p) {
            for (const auto& v : extra_vals) q.bind(p++, v);
        };

        int  total = 0;
        json items = json::array();
        auto pushShow = [](json& arr, SQLite::Statement& q) {
            json entry = {{"show_id",        q.getColumn(0).getString()},
                          {"title",          q.getColumn(1).getString()},
                          {"content_rating", q.getColumn(2).getString()},
                          {"episode_count",  q.getColumn(3).getInt()}};
            if (!q.getColumn(4).isNull()) entry["year"] = q.getColumn(4).getInt();
            arr.push_back(std::move(entry));
        };

        if (library_id.empty()) {
            SQLite::Statement cnt(db_.get(), "SELECT COUNT(*) FROM show s WHERE 1=1" + extras);
            int p = 1; bindExtras(cnt, p);
            if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

            SQLite::Statement q(db_.get(), R"(
                SELECT s.show_id, s.title, s.content_rating,
                       COUNT(e.episode_id) AS episode_count, s.year
                FROM show s LEFT JOIN episode e ON e.show_id = s.show_id
                WHERE 1=1)" + extras + R"( GROUP BY s.show_id ORDER BY s.title LIMIT ? OFFSET ?)");
            p = 1; bindExtras(q, p);
            q.bind(p++, limit); q.bind(p++, offset);
            while (q.executeStep()) pushShow(items, q);
        } else {
            SQLite::Statement cnt(db_.get(), R"(
                SELECT COUNT(DISTINCT s.show_id) FROM show s
                JOIN source_mapping sm ON sm.kairos_id = s.show_id
                    AND sm.item_type = 'show' AND sm.library_id = ?
                WHERE 1=1)" + extras);
            int p = 1; cnt.bind(p++, library_id); bindExtras(cnt, p);
            if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

            SQLite::Statement q(db_.get(), R"(
                SELECT s.show_id, s.title, s.content_rating,
                       COUNT(e.episode_id) AS episode_count, s.year
                FROM show s
                JOIN source_mapping sm ON sm.kairos_id = s.show_id
                    AND sm.item_type = 'show' AND sm.library_id = ?
                LEFT JOIN episode e ON e.show_id = s.show_id
                WHERE 1=1)" + extras + R"( GROUP BY s.show_id ORDER BY s.title LIMIT ? OFFSET ?)");
            p = 1; q.bind(p++, library_id); bindExtras(q, p);
            q.bind(p++, limit); q.bind(p++, offset);
            while (q.executeStep()) pushShow(items, q);
        }
        ok(res, json{{"items", items}, {"total", total}}.dump());
    });

    svr_.Get("/api/shows/:id/episodes", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        std::string season_filter;
        if (req.has_param("season")) season_filter = req.get_param_value("season");

        std::string where = season_filter.empty() ? "" : " AND season = " + season_filter;
        SQLite::Statement q(db_.get(),
            "SELECT episode_id, season, episode, title, duration_ms, overview, air_date, thumb "
            "FROM episode WHERE show_id = ?" + where + " ORDER BY season, episode");
        q.bind(1, id);
        json result = json::array();
        while (q.executeStep()) {
            result.push_back({
                {"episode_id",  q.getColumn(0).getString()},
                {"season",      q.getColumn(1).getInt()},
                {"episode",     q.getColumn(2).getInt()},
                {"title",       q.getColumn(3).getString()},
                {"duration_ms", q.getColumn(4).getInt64()},
                {"overview",    q.getColumn(5).getString()},
                {"air_date",    q.getColumn(6).getString()},
                {"thumb",       q.getColumn(7).getString()},
            });
        }
        ok(res, result.dump());
    });

    svr_.Get("/api/shows/:id/seasons", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement q(db_.get(),
            "SELECT DISTINCT season FROM episode WHERE show_id = ? ORDER BY season");
        q.bind(1, id);
        json seasons = json::array();
        while (q.executeStep()) seasons.push_back(q.getColumn(0).getInt());
        ok(res, json{{"seasons", seasons}}.dump());
    });

    svr_.Get("/api/episodes", [this](const Req& req, Res& res) {
        int         limit   = 50;
        int         offset  = 0;
        std::string show_id, search_q, season_p;
        if (req.has_param("limit"))   limit    = std::stoi(req.get_param_value("limit"));
        if (req.has_param("offset"))  offset   = std::stoi(req.get_param_value("offset"));
        if (req.has_param("show_id")) show_id  = req.get_param_value("show_id");
        if (req.has_param("q"))       search_q = req.get_param_value("q");
        if (req.has_param("season"))  season_p = req.get_param_value("season");

        std::string where = " WHERE 1=1";
        if (!show_id.empty())  where += " AND e.show_id = '" + show_id + "'";
        if (!season_p.empty()) where += " AND e.season = " + season_p;
        if (!search_q.empty()) where += " AND e.title LIKE '%' || ? || '%'";

        SQLite::Statement q(db_.get(), R"(
            SELECT e.episode_id, e.season, e.episode, e.title, e.duration_ms,
                   s.show_id, s.title AS show_title
            FROM episode e JOIN show s ON s.show_id = e.show_id
        )" + where + " ORDER BY s.title, e.season, e.episode LIMIT ? OFFSET ?");
        int p = 1;
        if (!search_q.empty()) q.bind(p++, search_q);
        q.bind(p++, limit); q.bind(p++, offset);

        json items = json::array();
        while (q.executeStep()) {
            items.push_back({
                {"episode_id",  q.getColumn(0).getString()},
                {"season",      q.getColumn(1).getInt()},
                {"episode",     q.getColumn(2).getInt()},
                {"title",       q.getColumn(3).getString()},
                {"duration_ms", q.getColumn(4).getInt64()},
                {"show_id",     q.getColumn(5).getString()},
                {"show_title",  q.getColumn(6).getString()},
            });
        }
        ok(res, json{{"items", items}}.dump());
    });

    svr_.Get("/api/movies", [this](const Req& req, Res& res) {
        int         limit      = 50, offset = 0;
        std::string library_id, search_q, genre, year_p, rating;
        if (req.has_param("limit"))          limit      = std::stoi(req.get_param_value("limit"));
        if (req.has_param("offset"))         offset     = std::stoi(req.get_param_value("offset"));
        if (req.has_param("library_id"))     library_id = req.get_param_value("library_id");
        if (req.has_param("q"))              search_q   = req.get_param_value("q");
        if (req.has_param("genre"))          genre      = req.get_param_value("genre");
        if (req.has_param("year"))           year_p     = req.get_param_value("year");
        if (req.has_param("content_rating")) rating     = req.get_param_value("content_rating");

        std::string extras;
        std::vector<std::string> extra_vals;
        if (!search_q.empty()) { extras += " AND m.title LIKE '%' || ? || '%'"; extra_vals.push_back(search_q); }
        if (!genre.empty())    appendJsonInClause("m", "genres",       genre,  extras, extra_vals);
        if (!year_p.empty())   { extras += " AND m.year = CAST(? AS INTEGER)"; extra_vals.push_back(year_p); }
        if (!rating.empty())   appendInClause("m.content_rating",      rating, extras, extra_vals);

        auto bindExtras = [&](SQLite::Statement& q, int& p) {
            for (const auto& v : extra_vals) q.bind(p++, v);
        };

        int  total = 0;
        json items = json::array();
        auto pushMovie = [](json& arr, SQLite::Statement& q) {
            auto entry = json{{"movie_id",       q.getColumn(0).getString()},
                              {"title",          q.getColumn(1).getString()},
                              {"content_rating", q.getColumn(2).getString()},
                              {"duration_ms",    q.getColumn(3).getInt64()}};
            if (!q.getColumn(4).isNull()) entry["year"] = q.getColumn(4).getInt();
            arr.push_back(std::move(entry));
        };

        if (library_id.empty()) {
            SQLite::Statement cnt(db_.get(), "SELECT COUNT(*) FROM movie m WHERE 1=1" + extras);
            int p = 1; bindExtras(cnt, p);
            if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

            SQLite::Statement q(db_.get(),
                "SELECT m.movie_id, m.title, m.content_rating, m.duration_ms, m.year "
                "FROM movie m WHERE 1=1" + extras + " ORDER BY m.title LIMIT ? OFFSET ?");
            p = 1; bindExtras(q, p);
            q.bind(p++, limit); q.bind(p++, offset);
            while (q.executeStep()) pushMovie(items, q);
        } else {
            SQLite::Statement cnt(db_.get(), R"(
                SELECT COUNT(DISTINCT m.movie_id) FROM movie m
                JOIN source_mapping sm ON sm.kairos_id = m.movie_id
                    AND sm.item_type = 'movie' AND sm.library_id = ?
                WHERE 1=1)" + extras);
            int p = 1; cnt.bind(p++, library_id); bindExtras(cnt, p);
            if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

            SQLite::Statement q(db_.get(), R"(
                SELECT m.movie_id, m.title, m.content_rating, m.duration_ms, m.year
                FROM movie m
                JOIN source_mapping sm ON sm.kairos_id = m.movie_id
                    AND sm.item_type = 'movie' AND sm.library_id = ?
                WHERE 1=1)" + extras + " ORDER BY m.title LIMIT ? OFFSET ?");
            p = 1; q.bind(p++, library_id); bindExtras(q, p);
            q.bind(p++, limit); q.bind(p++, offset);
            while (q.executeStep()) pushMovie(items, q);
        }
        ok(res, json{{"items", items}, {"total", total}}.dump());
    });

    // ── Show detail ────────────────────────────────────────────────────────────

    svr_.Get("/api/shows/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement q(db_.get(), R"(
            SELECT s.show_id, s.title, s.content_rating, s.overview, s.studio, s.status,
                   s.genres, s.thumb, s.art, s.imdb_id, s.tvdb_id, s.tmdb_id,
                   s.originally_available_at, s.year, s.audience_rating, s.locked,
                   COUNT(e.episode_id) AS episode_count
            FROM show s
            LEFT JOIN episode e ON e.show_id = s.show_id
            WHERE s.show_id = ?
            GROUP BY s.show_id
        )");
        q.bind(1, id);
        if (!q.executeStep()) { err(res, 404, "show not found"); return; }

        std::string external_id, source_id, source_base_url;
        SQLite::Statement sm(db_.get(), R"(
            SELECT sm.external_id, sm.source_id, ms.base_url
            FROM source_mapping sm
            JOIN media_source ms ON ms.source_id = sm.source_id
            WHERE sm.item_type = 'show' AND sm.kairos_id = ?
            LIMIT 1
        )");
        sm.bind(1, id);
        if (sm.executeStep()) {
            external_id     = sm.getColumn(0).getString();
            source_id       = sm.getColumn(1).getString();
            source_base_url = sm.getColumn(2).getString();
        }

        json show;
        show["show_id"]                  = q.getColumn(0).getString();
        show["title"]                    = q.getColumn(1).getString();
        show["content_rating"]           = q.getColumn(2).getString();
        show["overview"]                 = q.getColumn(3).getString();
        show["studio"]                   = q.getColumn(4).getString();
        show["status"]                   = q.getColumn(5).getString();
        try { show["genres"] = json::parse(q.getColumn(6).getString()); }
        catch (...) { show["genres"] = json::array(); }
        show["thumb"]                    = q.getColumn(7).getString();
        show["art"]                      = q.getColumn(8).getString();
        show["imdb_id"]                  = q.getColumn(9).getString();
        show["tvdb_id"]                  = q.getColumn(10).getString();
        show["tmdb_id"]                  = q.getColumn(11).getString();
        show["originally_available_at"]  = q.getColumn(12).getString();
        if (!q.getColumn(13).isNull()) show["year"] = q.getColumn(13).getInt();
        if (!q.getColumn(14).isNull()) show["audience_rating"] = q.getColumn(14).getDouble();
        show["locked"]                   = q.getColumn(15).getInt() != 0;
        show["episode_count"]            = q.getColumn(16).getInt();
        show["external_id"]              = external_id;
        show["source_id"]                = source_id;
        show["source_base_url"]          = source_base_url;
        ok(res, show.dump());
    });

    svr_.Patch("/api/shows/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            auto upd = [&](const char* col, const std::string& val) {
                SQLite::Statement s(db_.get(),
                    std::string("UPDATE show SET ") + col + " = ? WHERE show_id = ?");
                s.bind(1, val); s.bind(2, id); s.exec();
            };
            auto updN = [&](const char* col, int val) {
                SQLite::Statement s(db_.get(),
                    std::string("UPDATE show SET ") + col + " = ? WHERE show_id = ?");
                s.bind(1, val); s.bind(2, id); s.exec();
            };
            if (b.contains("title"))                   upd("title",                   b["title"].get<std::string>());
            if (b.contains("overview"))                upd("overview",                b["overview"].get<std::string>());
            if (b.contains("studio"))                  upd("studio",                  b["studio"].get<std::string>());
            if (b.contains("status"))                  upd("status",                  b["status"].get<std::string>());
            if (b.contains("content_rating"))          upd("content_rating",          b["content_rating"].get<std::string>());
            if (b.contains("originally_available_at")) upd("originally_available_at", b["originally_available_at"].get<std::string>());
            if (b.contains("imdb_id"))                 upd("imdb_id",                 b["imdb_id"].get<std::string>());
            if (b.contains("tvdb_id"))                 upd("tvdb_id",                 b["tvdb_id"].get<std::string>());
            if (b.contains("tmdb_id"))                 upd("tmdb_id",                 b["tmdb_id"].get<std::string>());
            if (b.contains("thumb"))                   upd("thumb",                   b["thumb"].get<std::string>());
            if (b.contains("art"))                     upd("art",                     b["art"].get<std::string>());
            if (b.contains("year"))                    updN("year",                   b["year"].get<int>());
            if (b.contains("genres"))                  upd("genres",                  b["genres"].is_array() ? b["genres"].dump() : b["genres"].get<std::string>());
            // Always lock after a manual edit so sync won't overwrite
            SQLite::Statement lk(db_.get(), "UPDATE show SET locked = 1 WHERE show_id = ?");
            lk.bind(1, id); lk.exec();
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Get("/api/shows/:id/thumb", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        std::string thumb, source_id;
        SQLite::Statement q(db_.get(), R"(
            SELECT s.thumb, sm.source_id FROM show s
            LEFT JOIN source_mapping sm ON sm.item_type = 'show' AND sm.kairos_id = s.show_id
            WHERE s.show_id = ? LIMIT 1
        )");
        q.bind(1, id);
        if (!q.executeStep() || (thumb = q.getColumn(0).getString()).empty()) {
            res.status = 404; return;
        }
        source_id = q.getColumn(1).getString();
        proxyImage(thumb, source_id, res);
    });

    svr_.Get("/api/shows/:id/art", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        std::string art, source_id;
        SQLite::Statement q(db_.get(), R"(
            SELECT s.art, sm.source_id FROM show s
            LEFT JOIN source_mapping sm ON sm.item_type = 'show' AND sm.kairos_id = s.show_id
            WHERE s.show_id = ? LIMIT 1
        )");
        q.bind(1, id);
        if (!q.executeStep() || (art = q.getColumn(0).getString()).empty()) {
            res.status = 404; return;
        }
        source_id = q.getColumn(1).getString();
        proxyImage(art, source_id, res);
    });

    svr_.Get("/api/episodes/:id/thumb", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        std::string thumb, source_id;
        SQLite::Statement q(db_.get(), R"(
            SELECT e.thumb, sm.source_id FROM episode e
            LEFT JOIN source_mapping sm ON sm.item_type = 'episode' AND sm.kairos_id = e.episode_id
            WHERE e.episode_id = ? LIMIT 1
        )");
        q.bind(1, id);
        if (!q.executeStep() || (thumb = q.getColumn(0).getString()).empty()) {
            res.status = 404; return;
        }
        source_id = q.getColumn(1).getString();
        proxyImage(thumb, source_id, res);
    });

    // ── Movie detail ───────────────────────────────────────────────────────────

    svr_.Get("/api/movies/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement q(db_.get(), R"(
            SELECT movie_id, title, content_rating, duration_ms, year,
                   overview, tagline, studio, director, genres, thumb, art,
                   imdb_id, tmdb_id, audience_rating, locked
            FROM movie WHERE movie_id = ?
        )");
        q.bind(1, id);
        if (!q.executeStep()) { err(res, 404, "movie not found"); return; }

        std::string external_id, source_id, source_base_url;
        SQLite::Statement sm(db_.get(), R"(
            SELECT sm.external_id, sm.source_id, ms.base_url
            FROM source_mapping sm
            JOIN media_source ms ON ms.source_id = sm.source_id
            WHERE sm.item_type = 'movie' AND sm.kairos_id = ?
            LIMIT 1
        )");
        sm.bind(1, id);
        if (sm.executeStep()) {
            external_id     = sm.getColumn(0).getString();
            source_id       = sm.getColumn(1).getString();
            source_base_url = sm.getColumn(2).getString();
        }

        json movie;
        movie["movie_id"]       = q.getColumn(0).getString();
        movie["title"]          = q.getColumn(1).getString();
        movie["content_rating"] = q.getColumn(2).getString();
        movie["duration_ms"]    = q.getColumn(3).getInt64();
        if (!q.getColumn(4).isNull()) movie["year"] = q.getColumn(4).getInt();
        movie["overview"]       = q.getColumn(5).getString();
        movie["tagline"]        = q.getColumn(6).getString();
        movie["studio"]         = q.getColumn(7).getString();
        movie["director"]       = q.getColumn(8).getString();
        try { movie["genres"] = json::parse(q.getColumn(9).getString()); }
        catch (...) { movie["genres"] = json::array(); }
        movie["thumb"]          = q.getColumn(10).getString();
        movie["art"]            = q.getColumn(11).getString();
        movie["imdb_id"]        = q.getColumn(12).getString();
        movie["tmdb_id"]        = q.getColumn(13).getString();
        if (!q.getColumn(14).isNull()) movie["audience_rating"] = q.getColumn(14).getDouble();
        movie["locked"]         = q.getColumn(15).getInt() != 0;
        movie["external_id"]    = external_id;
        movie["source_id"]      = source_id;
        movie["source_base_url"] = source_base_url;
        ok(res, movie.dump());
    });

    svr_.Patch("/api/movies/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            auto upd = [&](const char* col, const std::string& val) {
                SQLite::Statement s(db_.get(),
                    std::string("UPDATE movie SET ") + col + " = ? WHERE movie_id = ?");
                s.bind(1, val); s.bind(2, id); s.exec();
            };
            auto updN = [&](const char* col, int val) {
                SQLite::Statement s(db_.get(),
                    std::string("UPDATE movie SET ") + col + " = ? WHERE movie_id = ?");
                s.bind(1, val); s.bind(2, id); s.exec();
            };
            if (b.contains("title"))          upd("title",          b["title"].get<std::string>());
            if (b.contains("overview"))       upd("overview",       b["overview"].get<std::string>());
            if (b.contains("tagline"))        upd("tagline",        b["tagline"].get<std::string>());
            if (b.contains("studio"))         upd("studio",         b["studio"].get<std::string>());
            if (b.contains("director"))       upd("director",       b["director"].get<std::string>());
            if (b.contains("content_rating")) upd("content_rating", b["content_rating"].get<std::string>());
            if (b.contains("imdb_id"))        upd("imdb_id",        b["imdb_id"].get<std::string>());
            if (b.contains("tmdb_id"))        upd("tmdb_id",        b["tmdb_id"].get<std::string>());
            if (b.contains("thumb"))          upd("thumb",          b["thumb"].get<std::string>());
            if (b.contains("art"))            upd("art",            b["art"].get<std::string>());
            if (b.contains("year"))           updN("year",          b["year"].get<int>());
            if (b.contains("genres"))         upd("genres",         b["genres"].is_array() ? b["genres"].dump() : b["genres"].get<std::string>());
            SQLite::Statement lk(db_.get(), "UPDATE movie SET locked = 1 WHERE movie_id = ?");
            lk.bind(1, id); lk.exec();
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Get("/api/movies/:id/thumb", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        std::string thumb, source_id;
        SQLite::Statement q(db_.get(), R"(
            SELECT m.thumb, sm.source_id FROM movie m
            LEFT JOIN source_mapping sm ON sm.item_type = 'movie' AND sm.kairos_id = m.movie_id
            WHERE m.movie_id = ? LIMIT 1
        )");
        q.bind(1, id);
        if (!q.executeStep() || (thumb = q.getColumn(0).getString()).empty()) {
            res.status = 404; return;
        }
        source_id = q.getColumn(1).getString();
        proxyImage(thumb, source_id, res);
    });

    svr_.Get("/api/movies/:id/art", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        std::string art, source_id;
        SQLite::Statement q(db_.get(), R"(
            SELECT m.art, sm.source_id FROM movie m
            LEFT JOIN source_mapping sm ON sm.item_type = 'movie' AND sm.kairos_id = m.movie_id
            WHERE m.movie_id = ? LIMIT 1
        )");
        q.bind(1, id);
        if (!q.executeStep() || (art = q.getColumn(0).getString()).empty()) {
            res.status = 404; return;
        }
        source_id = q.getColumn(1).getString();
        proxyImage(art, source_id, res);
    });
}

// ---------------------------------------------------------------------------
// Image proxy helper
// ---------------------------------------------------------------------------

void Router::proxyImage(const std::string& imgPath, const std::string& sourceId,
                         httplib::Response& res) {
    // User-supplied URL — redirect, browser follows directly
    if (imgPath.rfind("http", 0) == 0) {
        res.set_redirect(imgPath);
        return;
    }

    // Plex relative path — need source credentials
    std::string base_url;
    SQLite::Statement q(db_.get(),
        "SELECT base_url FROM media_source WHERE source_id = ?");
    q.bind(1, sourceId);
    if (!q.executeStep() || (base_url = q.getColumn(0).getString()).empty()) {
        res.status = 404; return;
    }
    std::string token = conf_.token(sourceId);

    httplib::Client client(base_url);
    if (!token.empty())
        client.set_default_headers({{"X-Plex-Token", token}, {"Accept", "*/*"}});
    client.set_connection_timeout(10);
    client.set_read_timeout(15);

    auto img = client.Get(imgPath);
    if (!img || img->status != 200) { res.status = 502; return; }

    auto ct = img->get_header_value("Content-Type");
    if (ct.empty()) ct = "image/jpeg";
    res.set_content(img->body, ct);
}

// ---------------------------------------------------------------------------
// Shared Plex-list sync helper
// ---------------------------------------------------------------------------

void Router::syncPlexListItems(Res& res,
                                const std::string& list_type,
                                const std::string& list_id,
                                const std::string& source_id,
                                const std::string& external_id,
                                const std::string& plex_type) {
    std::string base_url, st;
    {
        SQLite::Statement sq(db_.get(),
            "SELECT base_url, source_type FROM media_source WHERE source_id = ?");
        sq.bind(1, source_id);
        if (!sq.executeStep()) { err(res, 404, "source not found"); return; }
        base_url = sq.getColumn(0).getString();
        st       = sq.getColumn(1).getString();
    }
    if (st != "plex") { err(res, 400, "source is not a Plex source"); return; }

    std::string token = conf_.token(source_id);
    httplib::Client client(base_url);
    client.set_default_headers({{"X-Plex-Token", token}, {"Accept", "application/json"}});
    client.set_connection_timeout(10);
    client.set_read_timeout(30);

    std::string path = (plex_type == "playlist")
        ? "/playlists/" + external_id + "/items"
        : "/library/metadata/" + external_id + "/children";
    auto r = client.Get(path.c_str());
    if (!r || r->status != 200) { err(res, 502, "failed to fetch items from Plex"); return; }

    struct Item { std::string item_type; std::string kairos_id; };
    std::vector<Item> items;
    int total = 0;
    try {
        auto j = json::parse(r->body);
        const auto& md = j["MediaContainer"];
        if (md.contains("Metadata")) {
            for (const auto& item : md["Metadata"]) {
                ++total;
                std::string pt = item.value("type", "");
                std::string it = (pt == "movie") ? "movie" : "episode";
                std::string rk = item["ratingKey"].get<std::string>();
                SQLite::Statement lk(db_.get(),
                    "SELECT kairos_id FROM source_mapping "
                    "WHERE source_id=? AND external_id=? AND item_type=?");
                lk.bind(1, source_id); lk.bind(2, rk); lk.bind(3, it);
                if (lk.executeStep()) items.push_back({ it, lk.getColumn(0).getString() });
            }
        }
    } catch (...) { err(res, 502, "failed to parse Plex response"); return; }

    // Replace items + upsert link record — single transaction
    const std::string fk_col   = (list_type == "playlist") ? "playlist_id"   : "filler_list_id";
    const std::string item_tbl = (list_type == "playlist") ? "playlist_item" : "filler_list_item";
    try {
        SQLite::Transaction txn(db_.get());

        SQLite::Statement del(db_.get(),
            "DELETE FROM " + item_tbl + " WHERE " + fk_col + " = ?");
        del.bind(1, list_id); del.exec();

        int pos = 0;
        for (const auto& item : items) {
            SQLite::Statement ins(db_.get(),
                "INSERT OR IGNORE INTO " + item_tbl +
                " (" + fk_col + ", position, item_type, item_id) VALUES (?,?,?,?)");
            ins.bind(1, list_id); ins.bind(2, pos++);
            ins.bind(3, item.item_type); ins.bind(4, item.kairos_id);
            ins.exec();
        }

        int64_t now = static_cast<int64_t>(std::time(nullptr));
        SQLite::Statement ul(db_.get(), R"(
            INSERT INTO plex_list_link (list_type, list_id, source_id, external_id, plex_type, last_synced_at)
            VALUES (?,?,?,?,?,?)
            ON CONFLICT(list_type, list_id) DO UPDATE SET
                source_id      = excluded.source_id,
                external_id    = excluded.external_id,
                plex_type      = excluded.plex_type,
                last_synced_at = excluded.last_synced_at
        )");
        ul.bind(1, list_type); ul.bind(2, list_id); ul.bind(3, source_id);
        ul.bind(4, external_id); ul.bind(5, plex_type); ul.bind(6, now);
        ul.exec();

        txn.commit();
    } catch (const std::exception& e) { err(res, 500, e.what()); return; }

    ok(res, json{{"synced", (int)items.size()}, {"total", total}}.dump());
}

// Playlists — ordered curated sequences of episodes/movies
// ---------------------------------------------------------------------------

void Router::registerPlaylistRoutes() {

    svr_.Get("/api/playlists", [this](const Req&, Res& res) {
        SQLite::Statement q(db_.get(), R"(
            SELECT p.playlist_id, p.title, p.mode,
                   COUNT(pi.id) AS item_count,
                   COALESCE(SUM(CASE pi.item_type
                       WHEN 'episode' THEN e.duration_ms
                       WHEN 'movie'   THEN m.duration_ms ELSE 0 END), 0) AS total_ms,
                   pll.source_id, pll.external_id, pll.plex_type, pll.last_synced_at
            FROM playlist p
            LEFT JOIN playlist_item pi ON pi.playlist_id = p.playlist_id
            LEFT JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
            LEFT JOIN movie   m ON pi.item_type = 'movie'   AND pi.item_id = m.movie_id
            LEFT JOIN plex_list_link pll ON pll.list_type = 'playlist' AND pll.list_id = p.playlist_id
            GROUP BY p.playlist_id ORDER BY p.title
        )");
        json result = json::array();
        while (q.executeStep()) {
            json entry = {
                {"playlist_id", q.getColumn(0).getString()},
                {"title",       q.getColumn(1).getString()},
                {"mode",        q.getColumn(2).getString()},
                {"item_count",  q.getColumn(3).getInt()},
                {"total_ms",    q.getColumn(4).getInt64()},
            };
            if (!q.getColumn(5).isNull()) {
                entry["plex_link"] = {
                    {"source_id",      q.getColumn(5).getString()},
                    {"external_id",    q.getColumn(6).getString()},
                    {"plex_type",      q.getColumn(7).getString()},
                    {"last_synced_at", q.getColumn(8).isNull()
                        ? json(nullptr) : json(q.getColumn(8).getInt64())},
                };
            }
            result.push_back(entry);
        }
        ok(res, result.dump());
    });

    // Sync all Plex-linked playlists across all sources (must register before /:id routes)
    svr_.Post("/api/playlists/plex-sync-all", [this](const Req&, Res& res) {
        sync_.triggerPlexLinkSync();
        res.status = 202;
        ok(res, json{{"status","accepted"}}.dump());
    });

    svr_.Post("/api/playlists", [this](const Req& req, Res& res) {
        try {
            auto b = json::parse(req.body);
            std::string title = b.value("title", "");
            if (title.empty()) { err(res, 400, "title required"); return; }
            std::string playlist_id = generateId();
            SQLite::Statement s(db_.get(),
                "INSERT INTO playlist (playlist_id, title, source) VALUES (?,?,'custom')");
            s.bind(1, playlist_id); s.bind(2, title); s.exec();
            res.status = 201;
            ok(res, json{{"playlist_id", playlist_id}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Get("/api/playlists/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement ph(db_.get(), "SELECT playlist_id, title, mode FROM playlist WHERE playlist_id = ?");
        ph.bind(1, id);
        if (!ph.executeStep()) { err(res, 404, "playlist not found"); return; }

        SQLite::Statement q(db_.get(), R"(
            SELECT pi.id, pi.position, pi.item_type, pi.item_id,
                   CASE pi.item_type
                       WHEN 'episode' THEN s.title || ' S' || PRINTF('%02d',e.season) ||
                                           'E' || PRINTF('%02d',e.episode) || ' — ' || e.title
                       WHEN 'movie'   THEN m.title ELSE ''
                   END AS title,
                   CASE pi.item_type
                       WHEN 'episode' THEN e.duration_ms
                       WHEN 'movie'   THEN m.duration_ms ELSE 0
                   END AS duration_ms,
                   e.season, e.episode
            FROM playlist_item pi
            LEFT JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
            LEFT JOIN show    s ON e.show_id = s.show_id
            LEFT JOIN movie   m ON pi.item_type = 'movie'   AND pi.item_id = m.movie_id
            WHERE pi.playlist_id = ? ORDER BY pi.position
        )");
        q.bind(1, id);
        json items = json::array();
        while (q.executeStep()) {
            json item = {
                {"id",          q.getColumn(0).getInt()},
                {"position",    q.getColumn(1).getInt()},
                {"item_type",   q.getColumn(2).getString()},
                {"item_id",     q.getColumn(3).getString()},
                {"title",       q.getColumn(4).getString()},
                {"duration_ms", q.getColumn(5).getInt64()},
            };
            if (!q.getColumn(6).isNull()) { item["season"] = q.getColumn(6).getInt(); item["episode"] = q.getColumn(7).getInt(); }
            items.push_back(item);
        }
        ok(res, json{{"playlist_id", ph.getColumn(0).getString()},
                     {"title",       ph.getColumn(1).getString()},
                     {"mode",        ph.getColumn(2).getString()},
                     {"items",       items}}.dump());
    });

    svr_.Post("/api/playlists/:id/plex-sync", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        try {
            auto b           = json::parse(req.body);
            std::string src  = b.value("source_id",   "");
            std::string ext  = b.value("external_id", "");
            std::string kind = b.value("plex_type",   "");
            if (src.empty() || ext.empty() || (kind != "playlist" && kind != "collection")) {
                err(res, 400, "source_id, external_id, plex_type required"); return;
            }
            syncPlexListItems(res, "playlist", id, src, ext, kind);
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    // Remove the Plex link from a playlist (makes it custom again; items are kept)
    svr_.Delete("/api/playlists/:id/plex-link", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement s(db_.get(),
            "DELETE FROM plex_list_link WHERE list_type = 'playlist' AND list_id = ?");
        s.bind(1, id); s.exec();
        res.status = 204; res.set_content("", "application/json");
    });

    svr_.Patch("/api/playlists/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            if (b.contains("title")) {
                SQLite::Statement s(db_.get(), "UPDATE playlist SET title = ? WHERE playlist_id = ?");
                s.bind(1, b["title"].get<std::string>()); s.bind(2, id); s.exec();
            }
            if (b.contains("mode")) {
                std::string mode = b["mode"].get<std::string>();
                if (mode != "sequential" && mode != "show_collection") {
                    err(res, 400, "mode must be 'sequential' or 'show_collection'"); return;
                }
                SQLite::Statement s(db_.get(), "UPDATE playlist SET mode = ? WHERE playlist_id = ?");
                s.bind(1, mode); s.bind(2, id); s.exec();
            }
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Delete("/api/playlists/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement s(db_.get(), "DELETE FROM playlist WHERE playlist_id = ?");
        s.bind(1, id); s.exec();
        ok(res, json{{"deleted", id}}.dump());
    });

    svr_.Post("/api/playlists/:id/items", [this](const Req& req, Res& res) {
        auto playlist_id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            std::string item_type = b.value("item_type", "episode");
            std::string item_id   = b.value("item_id",   "");
            if (item_id.empty()) { err(res, 400, "item_id required"); return; }

            int position = 0;
            {
                SQLite::Statement pq(db_.get(),
                    "SELECT COALESCE(MAX(position), -1) + 1 FROM playlist_item WHERE playlist_id = ?");
                pq.bind(1, playlist_id);
                if (pq.executeStep()) position = pq.getColumn(0).getInt();
            }
            SQLite::Statement s(db_.get(),
                "INSERT INTO playlist_item (playlist_id, position, item_type, item_id) VALUES (?,?,?,?)");
            s.bind(1, playlist_id); s.bind(2, position); s.bind(3, item_type); s.bind(4, item_id);
            s.exec();
            res.status = 201;
            ok(res, json{{"id", db_.get().getLastInsertRowid()}, {"position", position}}.dump());
        } catch (const SQLite::Exception& e) { err(res, 409, e.what()); }
          catch (const std::exception& e)    { err(res, 400, e.what()); }
    });

    // Bulk-add items — single transaction; skips duplicates
    svr_.Post("/api/playlists/:id/items/bulk", [this](const Req& req, Res& res) {
        auto playlist_id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            const auto& items = b.at("items");
            int added = 0;
            {
                SQLite::Statement pq(db_.get(),
                    "SELECT COALESCE(MAX(position), -1) + 1 FROM playlist_item WHERE playlist_id = ?");
                pq.bind(1, playlist_id);
                int position = pq.executeStep() ? pq.getColumn(0).getInt() : 0;
                SQLite::Transaction tx(db_.get());
                for (const auto& item : items) {
                    std::string item_type = item.value("item_type", "episode");
                    std::string item_id   = item.value("item_id",   "");
                    if (item_id.empty()) continue;
                    try {
                        SQLite::Statement s(db_.get(),
                            "INSERT INTO playlist_item (playlist_id, position, item_type, item_id) VALUES (?,?,?,?)");
                        s.bind(1, playlist_id); s.bind(2, position); s.bind(3, item_type); s.bind(4, item_id);
                        s.exec();
                        ++position; ++added;
                    } catch (const SQLite::Exception&) { /* skip duplicates */ }
                }
                tx.commit();
            }
            ok(res, json{{"added", added}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Delete("/api/playlists/:id/items/:iid", [this](const Req& req, Res& res) {
        auto playlist_id = req.path_params.at("id");
        auto iid         = std::stoi(req.path_params.at("iid"));
        int  pos         = -1;
        {
            SQLite::Statement q(db_.get(),
                "SELECT position FROM playlist_item WHERE id = ? AND playlist_id = ?");
            q.bind(1, iid); q.bind(2, playlist_id);
            if (!q.executeStep()) { err(res, 404, "item not found"); return; }
            pos = q.getColumn(0).getInt();
        }
        SQLite::Statement del(db_.get(), "DELETE FROM playlist_item WHERE id = ?");
        del.bind(1, iid); del.exec();
        SQLite::Statement ren(db_.get(),
            "UPDATE playlist_item SET position = position - 1 WHERE playlist_id = ? AND position > ?");
        ren.bind(1, playlist_id); ren.bind(2, pos); ren.exec();
        ok(res, json{{"deleted", iid}}.dump());
    });

    svr_.Patch("/api/playlists/:id/items/:iid", [this](const Req& req, Res& res) {
        auto playlist_id = req.path_params.at("id");
        auto iid         = std::stoi(req.path_params.at("iid"));
        try {
            auto b = json::parse(req.body);
            if (b.contains("position")) {
                int new_pos = b["position"].get<int>();
                SQLite::Statement s(db_.get(),
                    "UPDATE playlist_item SET position = ? WHERE id = ? AND playlist_id = ?");
                s.bind(1, new_pos); s.bind(2, iid); s.bind(3, playlist_id); s.exec();
            }
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });
}

// ---------------------------------------------------------------------------
// Filler lists — pools of short content for gap-filling
// ---------------------------------------------------------------------------

void Router::registerFillerRoutes() {

    svr_.Get("/api/filler-lists", [this](const Req&, Res& res) {
        SQLite::Statement q(db_.get(), R"(
            SELECT fl.filler_list_id, fl.title, fl.advancement,
                   COUNT(fi.id) AS item_count,
                   COALESCE(SUM(CASE fi.item_type
                       WHEN 'episode' THEN e.duration_ms
                       WHEN 'movie'   THEN m.duration_ms ELSE 0 END), 0) AS total_ms,
                   pll.source_id, pll.external_id, pll.plex_type, pll.last_synced_at
            FROM filler_list fl
            LEFT JOIN filler_list_item fi ON fi.filler_list_id = fl.filler_list_id
            LEFT JOIN episode e ON fi.item_type = 'episode' AND fi.item_id = e.episode_id
            LEFT JOIN movie   m ON fi.item_type = 'movie'   AND fi.item_id = m.movie_id
            LEFT JOIN plex_list_link pll ON pll.list_type = 'filler_list' AND pll.list_id = fl.filler_list_id
            GROUP BY fl.filler_list_id ORDER BY fl.title
        )");
        json result = json::array();
        while (q.executeStep()) {
            json entry = {
                {"filler_list_id", q.getColumn(0).getString()},
                {"title",          q.getColumn(1).getString()},
                {"advancement",    q.getColumn(2).getString()},
                {"item_count",     q.getColumn(3).getInt()},
                {"total_ms",       q.getColumn(4).getInt64()},
            };
            if (!q.getColumn(5).isNull()) {
                entry["plex_link"] = {
                    {"source_id",      q.getColumn(5).getString()},
                    {"external_id",    q.getColumn(6).getString()},
                    {"plex_type",      q.getColumn(7).getString()},
                    {"last_synced_at", q.getColumn(8).isNull()
                        ? json(nullptr) : json(q.getColumn(8).getInt64())},
                };
            }
            result.push_back(entry);
        }
        ok(res, result.dump());
    });

    // Sync all Plex-linked filler lists (must register before /:id routes)
    svr_.Post("/api/filler-lists/plex-sync-all", [this](const Req&, Res& res) {
        sync_.triggerPlexLinkSync();
        res.status = 202;
        ok(res, json{{"status","accepted"}}.dump());
    });

    svr_.Post("/api/filler-lists", [this](const Req& req, Res& res) {
        try {
            auto b = json::parse(req.body);
            std::string title       = b.value("title",       "");
            std::string advancement = b.value("advancement", "shuffle");
            if (title.empty()) { err(res, 400, "title required"); return; }
            std::string fid = generateId();
            SQLite::Statement s(db_.get(),
                "INSERT INTO filler_list (filler_list_id, title, advancement) VALUES (?,?,?)");
            s.bind(1, fid); s.bind(2, title); s.bind(3, advancement); s.exec();
            res.status = 201;
            ok(res, json{{"filler_list_id", fid}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Get("/api/filler-lists/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement fh(db_.get(),
            "SELECT filler_list_id, title, advancement FROM filler_list WHERE filler_list_id = ?");
        fh.bind(1, id);
        if (!fh.executeStep()) { err(res, 404, "filler list not found"); return; }

        SQLite::Statement q(db_.get(), R"(
            SELECT fi.id, fi.item_type, fi.item_id, fi.position,
                   CASE fi.item_type
                       WHEN 'episode' THEN s.title || ' S' || PRINTF('%02d',e.season) ||
                                           'E' || PRINTF('%02d',e.episode) || ' — ' || e.title
                       WHEN 'movie'   THEN m.title ELSE ''
                   END AS title,
                   CASE fi.item_type
                       WHEN 'episode' THEN e.duration_ms
                       WHEN 'movie'   THEN m.duration_ms ELSE 0
                   END AS duration_ms
            FROM filler_list_item fi
            LEFT JOIN episode e ON fi.item_type = 'episode' AND fi.item_id = e.episode_id
            LEFT JOIN show    s ON e.show_id = s.show_id
            LEFT JOIN movie   m ON fi.item_type = 'movie'   AND fi.item_id = m.movie_id
            WHERE fi.filler_list_id = ? ORDER BY fi.position
        )");
        q.bind(1, id);
        json items = json::array();
        while (q.executeStep()) {
            items.push_back({
                {"id",          q.getColumn(0).getInt()},
                {"item_type",   q.getColumn(1).getString()},
                {"item_id",     q.getColumn(2).getString()},
                {"position",    q.getColumn(3).getInt()},
                {"title",       q.getColumn(4).getString()},
                {"duration_ms", q.getColumn(5).getInt64()},
            });
        }
        ok(res, json{{"filler_list_id", fh.getColumn(0).getString()},
                     {"title",          fh.getColumn(1).getString()},
                     {"advancement",    fh.getColumn(2).getString()},
                     {"items",          items}}.dump());
    });

    svr_.Post("/api/filler-lists/:id/plex-sync", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        try {
            auto b           = json::parse(req.body);
            std::string src  = b.value("source_id",   "");
            std::string ext  = b.value("external_id", "");
            std::string kind = b.value("plex_type",   "");
            if (src.empty() || ext.empty() || (kind != "playlist" && kind != "collection")) {
                err(res, 400, "source_id, external_id, plex_type required"); return;
            }
            syncPlexListItems(res, "filler_list", id, src, ext, kind);
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    // Remove the Plex link from a filler list
    svr_.Delete("/api/filler-lists/:id/plex-link", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement s(db_.get(),
            "DELETE FROM plex_list_link WHERE list_type = 'filler_list' AND list_id = ?");
        s.bind(1, id); s.exec();
        res.status = 204; res.set_content("", "application/json");
    });

    svr_.Patch("/api/filler-lists/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            if (b.contains("title")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE filler_list SET title = ? WHERE filler_list_id = ?");
                s.bind(1, b["title"].get<std::string>()); s.bind(2, id); s.exec();
            }
            if (b.contains("advancement")) {
                SQLite::Statement s(db_.get(),
                    "UPDATE filler_list SET advancement = ? WHERE filler_list_id = ?");
                s.bind(1, b["advancement"].get<std::string>()); s.bind(2, id); s.exec();
            }
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Delete("/api/filler-lists/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement s(db_.get(), "DELETE FROM filler_list WHERE filler_list_id = ?");
        s.bind(1, id); s.exec();
        ok(res, json{{"deleted", id}}.dump());
    });

    svr_.Post("/api/filler-lists/:id/items", [this](const Req& req, Res& res) {
        auto fid = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            std::string item_type = b.value("item_type", "episode");
            std::string item_id   = b.value("item_id",   "");
            if (item_id.empty()) { err(res, 400, "item_id required"); return; }

            int position = 0;
            {
                SQLite::Statement pq(db_.get(),
                    "SELECT COALESCE(MAX(position), -1) + 1 FROM filler_list_item WHERE filler_list_id = ?");
                pq.bind(1, fid);
                if (pq.executeStep()) position = pq.getColumn(0).getInt();
            }
            SQLite::Statement s(db_.get(), R"(
                INSERT INTO filler_list_item (filler_list_id, item_type, item_id, position)
                VALUES (?,?,?,?)
            )");
            s.bind(1, fid); s.bind(2, item_type); s.bind(3, item_id); s.bind(4, position);
            s.exec();
            res.status = 201;
            ok(res, json{{"id", db_.get().getLastInsertRowid()}, {"position", position}}.dump());
        } catch (const SQLite::Exception& e) { err(res, 409, e.what()); }
          catch (const std::exception& e)    { err(res, 400, e.what()); }
    });

    svr_.Delete("/api/filler-lists/:id/items/:iid", [this](const Req& req, Res& res) {
        auto iid = std::stoi(req.path_params.at("iid"));
        SQLite::Statement s(db_.get(), "DELETE FROM filler_list_item WHERE id = ?");
        s.bind(1, iid); s.exec();
        ok(res, json{{"deleted", iid}}.dump());
    });

    // Bulk-add items — single transaction; skips duplicates
    svr_.Post("/api/filler-lists/:id/items/bulk", [this](const Req& req, Res& res) {
        auto fid = req.path_params.at("id");
        try {
            auto b = json::parse(req.body);
            const auto& items = b.at("items");
            int added = 0;
            {
                SQLite::Statement pq(db_.get(),
                    "SELECT COALESCE(MAX(position), -1) + 1 FROM filler_list_item WHERE filler_list_id = ?");
                pq.bind(1, fid);
                int position = pq.executeStep() ? pq.getColumn(0).getInt() : 0;
                SQLite::Transaction tx(db_.get());
                for (const auto& item : items) {
                    std::string item_type = item.value("item_type", "episode");
                    std::string item_id   = item.value("item_id",   "");
                    if (item_id.empty()) continue;
                    try {
                        SQLite::Statement s(db_.get(), R"(
                            INSERT INTO filler_list_item (filler_list_id, item_type, item_id, position)
                            VALUES (?,?,?,?))");
                        s.bind(1, fid); s.bind(2, item_type); s.bind(3, item_id); s.bind(4, position);
                        s.exec();
                        ++position; ++added;
                    } catch (const SQLite::Exception&) { /* skip duplicates */ }
                }
                tx.commit();
            }
            ok(res, json{{"added", added}}.dump());
        } catch (const std::exception& e) { err(res, 400, e.what()); }
    });
}

// ---------------------------------------------------------------------------
// Activity — sync-all trigger + SSE log stream
// ---------------------------------------------------------------------------

void Router::registerActivityRoutes() {

    svr_.Post("/api/sync/all", [this](const Req&, Res& res) {
        sync_.triggerSync("");  // empty = sync all sources
        res.status = 202;
        ok(res, json{{"status", "started"}}.dump());
    });

    svr_.Get("/api/logs/stream", [this](const Req&, Res& res) {
        res.set_header("Cache-Control",      "no-cache");
        res.set_header("Connection",         "keep-alive");
        res.set_header("X-Accel-Buffering",  "no");
        res.set_header("Access-Control-Allow-Origin", "*");

        res.set_chunked_content_provider("text/event-stream",
            [this, cur_seq = uint64_t{0}, sent_init = false]
            (size_t, httplib::DataSink& sink) mutable -> bool {

                if (!sent_init) {
                    sent_init = true;
                    auto [lines, seq] = logs_.recent(200);
                    cur_seq = seq;
                    for (const auto& line : lines) {
                        std::string ev = "data:" + line + "\n\n";
                        if (!sink.write(ev.data(), ev.size())) return false;
                    }
                    return true;
                }

                auto [new_lines, new_seq] =
                    logs_.waitAfter(cur_seq, std::chrono::milliseconds{25'000});

                if (!sink.is_writable()) return false;

                if (new_lines.empty()) {
                    static const std::string ping = ": ping\n\n";
                    return sink.write(ping.data(), ping.size());
                }

                cur_seq = new_seq;
                for (const auto& line : new_lines) {
                    std::string ev = "data:" + line + "\n\n";
                    if (!sink.write(ev.data(), ev.size())) return false;
                }
                return true;
            });
    });
}

// ---------------------------------------------------------------------------
// Downloads — yt-dlp job management
// ---------------------------------------------------------------------------

void Router::registerDownloadRoutes() {

    svr_.Get("/api/config/download", [this](const Req&, Res& res) {
        ok(res, json{{"path", conf_.getDownloadPath()}}.dump());
    });

    svr_.Put("/api/config/download", [this](const Req& req, Res& res) {
        try {
            auto b    = json::parse(req.body);
            auto path = b.value("path", "");
            conf_.setDownloadPath(path);
            ok(res, json{{"ok", true}}.dump());
        } catch (const json::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Post("/api/download/jobs", [this](const Req& req, Res& res) {
        try {
            auto b    = json::parse(req.body);
            auto url  = b.value("url", "");
            auto path = b.contains("path") && b["path"].is_string() && !b["path"].get<std::string>().empty()
                            ? b["path"].get<std::string>()
                            : conf_.getDownloadPath();
            if (url.empty())  { err(res, 400, "url required");                    return; }
            if (path.empty()) { err(res, 400, "download path not configured");    return; }
            auto id = dl_.startJob(url, path);
            ok(res, json{{"job_id", id}}.dump());
        } catch (const json::exception& e) { err(res, 400, e.what()); }
    });

    svr_.Get("/api/download/jobs", [this](const Req&, Res& res) {
        auto jobs = dl_.getJobs();
        json result = json::array();
        for (const auto& j : jobs) {
            json log = json::array();
            for (const auto& line : j.log) log.push_back(line);
            result.push_back({
                {"id",         j.id},
                {"url",        j.url},
                {"dest_path",  j.dest_path},
                {"status",     j.status},
                {"progress",   j.progress},
                {"log",        log},
                {"started_at", j.started_at},
            });
        }
        ok(res, result.dump());
    });
}

// ---------------------------------------------------------------------------
// Scheduler — rules engine, EPG, M3U
// ---------------------------------------------------------------------------

void Router::registerSchedulerRoutes() {

    // ── M3U channel list ─────────────────────────────────────────────────────
    svr_.Get("/playlist.m3u", [this](const Req& req, Res& res) {
        std::string host = req.get_header_value("Host");
        if (host.empty()) host = "localhost:8080";
        auto m3u = materializer_.generateM3U("http://" + host);
        res.set_content(m3u, "application/x-mpegURL");
    });

    // ── XMLTV EPG ────────────────────────────────────────────────────────────
    svr_.Get("/epg.xml", [this](const Req& req, Res& res) {
        int hours = 24;
        if (req.has_param("hours")) {
            try { hours = std::stoi(req.get_param_value("hours")); } catch (...) {}
        }
        hours = std::max(1, std::min(hours, 72));
        auto xml = materializer_.generateXMLTV(hours);
        res.set_content(xml, "application/xml");
    });

    // ── What's playing now on a channel ──────────────────────────────────────
    svr_.Get(R"(/api/channels/([^/]+)/now)", [this](const Req& req, Res& res) {
      try {
        std::string channel_id = req.matches[1];
        auto t = std::time(nullptr);

        // Ensure the EPG cache covers a window around the current time so that
        // mid-episode joins and channels that have been idle for days all get a
        // wall-clock-accurate answer. Project from 1 hour ago so any episode
        // that started before now is already in the cache.
        materializer_.ensureScheduled(channel_id, t - 3600, 4);

        // Look up which scheduled entry is airing right now.
        {
            SQLite::Statement q(db_.get(), R"(
                SELECT sp.item_type, sp.item_id,
                       COALESCE(sp.block_id, '')          AS block_id,
                       sp.wall_clock_start,
                       COALESCE(e.title,  m.title,  '')   AS title,
                       COALESCE(s.title,  '')             AS show_title,
                       COALESCE(e.show_id,'')             AS show_id,
                       COALESCE(e.season, 0)              AS season,
                       COALESCE(e.episode,0)              AS ep_num,
                       COALESCE(e.file_path, m.file_path,'') AS file_path,
                       COALESCE(e.duration_ms, m.duration_ms, 0) AS duration_ms
                FROM scheduled_program sp
                LEFT JOIN episode e ON sp.item_type='episode' AND sp.item_id=e.episode_id
                LEFT JOIN show    s ON sp.item_type='episode' AND e.show_id =s.show_id
                LEFT JOIN movie   m ON sp.item_type='movie'   AND sp.item_id=m.movie_id
                WHERE sp.channel_id    = ?
                  AND sp.wall_clock_start <= ?
                  AND sp.wall_clock_end   >  ?
                  AND sp.status != 'skipped'
                ORDER BY sp.wall_clock_start DESC
                LIMIT 1
            )");
            q.bind(1, channel_id);
            q.bind(2, static_cast<int64_t>(t));
            q.bind(3, static_cast<int64_t>(t));
            if (q.executeStep()) {
                std::string item_type  = q.getColumn(0).getString();
                std::string item_id    = q.getColumn(1).getString();
                std::string block_id   = q.getColumn(2).getString();
                int64_t     wall_start = q.getColumn(3).getInt64();
                std::string title      = q.getColumn(4).getString();
                std::string show_title = q.getColumn(5).getString();
                std::string show_id    = q.getColumn(6).getString();
                int         season     = q.getColumn(7).getInt();
                int         ep_num     = q.getColumn(8).getInt();
                std::string file_path  = q.getColumn(9).getString();
                int64_t     duration_ms= q.getColumn(10).getInt64();

                json j = {
                    {"item_type",           item_type},
                    {"item_id",             item_id},
                    {"file_path",           conf_.applyPathMap(file_path)},
                    {"duration_ms",         duration_ms},
                    {"title",               title},
                    {"block_id",            block_id},
                    {"wall_clock_start_ms", wall_start * 1000LL},
                };
                if (!show_title.empty()) {
                    j["show_title"]  = show_title;
                    j["show_id"]     = show_id;
                    j["season"]      = season;
                    j["episode_num"] = ep_num;
                }
                try {
                    SQLite::Statement sm(db_.get(),
                        "SELECT source_id, external_id FROM source_mapping "
                        "WHERE kairos_id = ? LIMIT 1");
                    sm.bind(1, item_id);
                    if (sm.executeStep()) {
                        j["source_id"]   = sm.getColumn(0).getString();
                        j["external_id"] = sm.getColumn(1).getString();
                    }
                } catch (...) {}
                ok(res, j.dump());
                return;
            }
        }

        // No cache entry for the current time — try live block resolution.
        auto block_opt = engine_.resolveBlock(channel_id, t);
        std::optional<ScheduledItem> item_opt;
        if (block_opt)
            item_opt = engine_.nextItem(channel_id, *block_opt, std::time(nullptr));

        if (block_opt && item_opt) {
            const auto& item = *item_opt;
            json j = {
                {"item_type",            item.item_type},
                {"item_id",              item.item_id},
                {"file_path",            conf_.applyPathMap(item.file_path)},
                {"duration_ms",          item.duration_ms},
                {"title",                item.title},
                {"block_id",             item.block_id},
                {"wall_clock_start_ms",  static_cast<int64_t>(t) * 1000},
            };
            if (!item.show_title.empty()) {
                j["show_title"]  = item.show_title;
                j["show_id"]     = item.show_id;
                j["season"]      = item.season;
                j["episode_num"] = item.episode_num;
            }
            try {
                SQLite::Statement sm(db_.get(),
                    "SELECT source_id, external_id FROM source_mapping "
                    "WHERE kairos_id = ? LIMIT 1");
                sm.bind(1, item.item_id);
                if (sm.executeStep()) {
                    j["source_id"]   = sm.getColumn(0).getString();
                    j["external_id"] = sm.getColumn(1).getString();
                }
            } catch (...) {}
            ok(res, j.dump());
            return;
        }

        // Fallback 1: random item from the channel's default filler lists.
        {
            SQLite::Statement fq(db_.get(), R"(
                SELECT fi.item_type, fi.item_id,
                       COALESCE(e.file_path, m.file_path, '') AS file_path,
                       COALESCE(e.title,     m.title,     '') AS title,
                       COALESCE(e.duration_ms, m.duration_ms, 0) AS duration_ms
                FROM channel_filler_entry cfe
                JOIN filler_list_item fi ON fi.filler_list_id = cfe.filler_list_id
                LEFT JOIN episode e ON fi.item_type='episode' AND fi.item_id=e.episode_id
                LEFT JOIN movie   m ON fi.item_type='movie'   AND fi.item_id=m.movie_id
                WHERE cfe.channel_id = ?
                  AND (e.file_path IS NOT NULL OR m.file_path IS NOT NULL)
                ORDER BY RANDOM()
                LIMIT 1
            )");
            fq.bind(1, channel_id);
            if (fq.executeStep()) {
                json j = {
                    {"item_type",           fq.getColumn(0).getString()},
                    {"item_id",             fq.getColumn(1).getString()},
                    {"file_path",           conf_.applyPathMap(fq.getColumn(2).getString())},
                    {"title",               fq.getColumn(3).getString()},
                    {"duration_ms",         fq.getColumn(4).getInt64()},
                    {"block_id",            ""},
                    {"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
                };
                ok(res, j.dump());
                return;
            }
        }

        // Fallback 2: channel-configured offline screen (video or image+audio).
        {
            SQLite::Statement oq(db_.get(),
                "SELECT offline_video_path, offline_image_path, "
                "       offline_audio_id, offline_audio_type "
                "FROM channel WHERE channel_id = ?");
            oq.bind(1, channel_id);
            if (oq.executeStep()) {
                std::string vid_path  = oq.getColumn(0).getString();
                std::string img_path  = oq.getColumn(1).getString();
                std::string audio_id  = oq.getColumn(2).getString();
                std::string audio_typ = oq.getColumn(3).getString();

                if (!vid_path.empty()) {
                    json j = {
                        {"item_type",           "offline"},
                        {"file_path",           conf_.applyPathMap(vid_path)},
                        {"duration_ms",         0},
                        {"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
                    };
                    ok(res, j.dump());
                    return;
                }
                if (!img_path.empty()) {
                    json j = {
                        {"item_type",           "offline"},
                        {"offline_image_path",  conf_.applyPathMap(img_path)},
                        {"duration_ms",         0},
                        {"wall_clock_start_ms", static_cast<int64_t>(t) * 1000},
                    };
                    if (!audio_id.empty() && !audio_typ.empty()) {
                        const char* sql = (audio_typ == "episode")
                            ? "SELECT file_path FROM episode WHERE episode_id = ?"
                            : "SELECT file_path FROM movie   WHERE movie_id   = ?";
                        SQLite::Statement aq(db_.get(), sql);
                        aq.bind(1, audio_id);
                        if (aq.executeStep()) {
                            std::string ap = aq.getColumn(0).getString();
                            if (!ap.empty()) j["offline_audio_path"] = conf_.applyPathMap(ap);
                        }
                    }
                    ok(res, j.dump());
                    return;
                }
            }
        }

        err(res, 404, "no content or fallback configured for this channel");
      } catch (const std::exception& e) {
        logErr("GET /api/channels/now", e); err(res, 500, e.what());
      }
    });

    // ── What's playing next on a channel ─────────────────────────────────────
    svr_.Get(R"(/api/channels/([^/]+)/next)", [this](const Req& req, Res& res) {
      try {
        std::string channel_id = req.matches[1];
        auto t = std::time(nullptr);

        materializer_.ensureScheduled(channel_id, t, 4);

        // Find when the currently-airing item ends.
        int64_t current_end = static_cast<int64_t>(t);
        {
            SQLite::Statement q(db_.get(), R"(
                SELECT wall_clock_end FROM scheduled_program
                WHERE channel_id = ? AND wall_clock_start <= ? AND wall_clock_end > ?
                  AND status != 'skipped'
                ORDER BY wall_clock_start DESC LIMIT 1
            )");
            q.bind(1, channel_id);
            q.bind(2, static_cast<int64_t>(t));
            q.bind(3, static_cast<int64_t>(t));
            if (q.executeStep()) current_end = q.getColumn(0).getInt64();
        }

        // Return the next non-filler scheduled item starting at or after current_end.
        SQLite::Statement q(db_.get(), R"(
            SELECT sp.item_type, sp.item_id, COALESCE(sp.block_id, ''),
                   sp.wall_clock_start,
                   COALESCE(e.title, m.title, '')    AS title,
                   COALESCE(s.title, '')              AS show_title,
                   COALESCE(e.show_id, '')            AS show_id,
                   COALESCE(e.season,  0)             AS season,
                   COALESCE(e.episode, 0)             AS ep_num,
                   COALESCE(e.file_path, m.file_path, '') AS file_path,
                   COALESCE(e.duration_ms, m.duration_ms,
                            (sp.wall_clock_end - sp.wall_clock_start) * 1000) AS duration_ms
            FROM scheduled_program sp
            LEFT JOIN episode e ON sp.item_type = 'episode' AND sp.item_id = e.episode_id
            LEFT JOIN show    s ON sp.item_type = 'episode' AND e.show_id  = s.show_id
            LEFT JOIN movie   m ON sp.item_type = 'movie'   AND sp.item_id = m.movie_id
            WHERE sp.channel_id = ?
              AND sp.wall_clock_start >= ?
              AND sp.is_filler = 0
              AND sp.status != 'skipped'
            ORDER BY sp.wall_clock_start
            LIMIT 1
        )");
        q.bind(1, channel_id);
        q.bind(2, current_end);

        if (!q.executeStep()) { err(res, 404, "no next item available"); return; }

        std::string item_type   = q.getColumn(0).getString();
        std::string item_id     = q.getColumn(1).getString();
        std::string block_id    = q.getColumn(2).getString();
        int64_t     wall_start  = q.getColumn(3).getInt64();
        std::string title       = q.getColumn(4).getString();
        std::string show_title  = q.getColumn(5).getString();
        std::string show_id     = q.getColumn(6).getString();
        int         season      = q.getColumn(7).getInt();
        int         ep_num      = q.getColumn(8).getInt();
        std::string file_path   = q.getColumn(9).getString();
        int64_t     duration_ms = q.getColumn(10).getInt64();

        json j = {
            {"item_type",           item_type},
            {"item_id",             item_id},
            {"file_path",           conf_.applyPathMap(file_path)},
            {"duration_ms",         duration_ms},
            {"title",               title},
            {"block_id",            block_id},
            {"wall_clock_start_ms", wall_start * 1000LL},
        };
        if (!show_title.empty()) {
            j["show_title"]  = show_title;
            j["show_id"]     = show_id;
            j["season"]      = season;
            j["episode_num"] = ep_num;
        }
        try {
            SQLite::Statement sm(db_.get(),
                "SELECT source_id, external_id FROM source_mapping WHERE kairos_id = ? LIMIT 1");
            sm.bind(1, item_id);
            if (sm.executeStep()) {
                j["source_id"]   = sm.getColumn(0).getString();
                j["external_id"] = sm.getColumn(1).getString();
            }
        } catch (...) {}
        ok(res, j.dump());
      } catch (const std::exception& e) {
        logErr("GET /api/channels/next", e); err(res, 500, e.what());
      }
    });

    // ── Report playback completion ───────────────────────────────────────────
    //  Body: { item_type, item_id, block_id, duration_actual_ms }
    svr_.Post(R"(/api/channels/([^/]+)/played)", [this](const Req& req, Res& res) {
        try {
            std::string channel_id = req.matches[1];
            auto b = json::parse(req.body);
            std::string item_type = b.value("item_type", "episode");
            std::string item_id   = b.value("item_id",   "");
            std::string block_id  = b.value("block_id",  "");
            int64_t duration_ms   = b.value("duration_actual_ms", int64_t(0));

            if (item_id.empty()) { err(res, 400, "item_id required"); return; }
            engine_.markPlayed(channel_id, block_id, item_type, item_id, duration_ms);
            materializer_.notifyPlayed(channel_id, item_id);
            // on_play channels: markPlayed() advanced the cursor, so the cached
            // schedule is now stale and must be regenerated for the next episode.
            {
                SQLite::Statement am(db_.get(),
                    "SELECT advance_mode FROM channel WHERE channel_id=?");
                am.bind(1, channel_id);
                if (am.executeStep() && am.getColumn(0).getString() == "on_play")
                    clearScheduleCache(channel_id);
            }
            ok(res, json{{"ok", true}}.dump());
        } catch (const std::exception& e) {
            err(res, 400, e.what());
        }
    });

    // ── EPG preview — returns cached schedule if available, else in-memory projection ──
    // Never calls ensureScheduled(); only reads cache or simulates, no DB writes.
    svr_.Get(R"(/api/channels/([^/]+)/epg/preview)", [this](const Req& req, Res& res) {
      try {
        std::string channel_id = req.matches[1];
        int hours = 48;
        if (req.has_param("hours")) {
            try { hours = std::stoi(req.get_param_value("hours")); } catch (...) {}
        }
        hours = std::max(1, std::min(hours, 168));

        auto now     = std::time(nullptr);
        auto horizon = static_cast<int64_t>(now + hours * 3600LL);

        // ?seed=N forces a fresh in-SAVEPOINT projection with that seed regardless of cache.
        int  req_seed  = -1;
        bool has_seed  = req.has_param("seed");
        if (has_seed) {
            try { req_seed = std::stoi(req.get_param_value("seed")); } catch (...) {}
        }

        // Check if the cache already has entries for this channel in the window.
        bool force = has_seed || (req.has_param("force") && req.get_param_value("force") == "1");
        bool has_cache = false;
        if (!force) {
            SQLite::Statement ck(db_.get(), R"(
                SELECT 1 FROM scheduled_program
                WHERE channel_id=? AND wall_clock_start >= ? AND wall_clock_start < ?
                  AND status != 'skipped'
                LIMIT 1
            )");
            ck.bind(1, channel_id);
            ck.bind(2, static_cast<int64_t>(now));
            ck.bind(3, horizon);
            has_cache = ck.executeStep();
        }

        json arr = json::array();

        if (has_cache) {
            // Return cached schedule. Filler items are now persisted in
            // scheduled_program so they appear naturally alongside content items.
            SQLite::Statement q(db_.get(), R"(
                SELECT sp.item_type, sp.item_id, sp.block_id,
                       sp.wall_clock_start, sp.wall_clock_end, sp.status,
                       COALESCE(e.title, m.title, '') AS item_title,
                       COALESCE(s.title, '')           AS show_title,
                       COALESCE(e.show_id, '')         AS show_id,
                       COALESCE(e.season,  0)          AS season,
                       COALESCE(e.episode, 0)          AS ep_num,
                       COALESCE(e.duration_ms, m.duration_ms,
                                (sp.wall_clock_end - sp.wall_clock_start) * 1000)
                           AS duration_ms
                FROM scheduled_program sp
                LEFT JOIN episode e ON sp.item_type='episode' AND sp.item_id=e.episode_id
                LEFT JOIN show    s ON sp.item_type='episode' AND e.show_id=s.show_id
                LEFT JOIN movie   m ON sp.item_type='movie'   AND sp.item_id=m.movie_id
                WHERE sp.channel_id=?
                  AND sp.wall_clock_start >= ?
                  AND sp.wall_clock_start <  ?
                  AND sp.status != 'skipped'
                ORDER BY sp.wall_clock_start
            )");
            q.bind(1, channel_id);
            q.bind(2, static_cast<int64_t>(now));
            q.bind(3, horizon);
            while (q.executeStep()) {
                json j = {
                    {"item_type",           q.getColumn(0).getString()},
                    {"item_id",             q.getColumn(1).getString()},
                    {"block_id",            q.getColumn(2).isNull() ? "" : q.getColumn(2).getString()},
                    {"wall_clock_start_ms", q.getColumn(3).getInt64() * 1000},
                    {"wall_clock_end_ms",   q.getColumn(4).getInt64() * 1000},
                    {"status",              q.getColumn(5).getString()},
                    {"title",               q.getColumn(6).getString()},
                    {"duration_ms",         q.getColumn(11).getInt64()},
                };
                std::string show_title = q.getColumn(7).getString();
                if (!show_title.empty()) {
                    j["show_title"]  = show_title;
                    j["show_id"]     = q.getColumn(8).getString();
                    j["season"]      = q.getColumn(9).getInt();
                    j["episode_num"] = q.getColumn(10).getInt();
                }
                arr.push_back(j);
            }
        } else {
            // No persistent cache — run projection inside a SAVEPOINT so all
            // DB writes (play_history + scheduled_program) are rolled back after
            // we read the results, leaving live state untouched.
            db_.get().exec("SAVEPOINT preview_sp");
            try {
                if (has_seed) {
                    // Wipe the channel's cache within the SAVEPOINT so the seed
                    // produces a fully fresh lineup rather than extending.
                    SQLite::Statement del(db_.get(),
                        "DELETE FROM scheduled_program WHERE channel_id=?");
                    del.bind(1, channel_id);
                    del.exec();
                    SQLite::Statement del2(db_.get(),
                        "DELETE FROM play_history WHERE channel_id=? AND is_scheduled=1");
                    del2.bind(1, channel_id);
                    del2.exec();
                }
                materializer_.ensureScheduled(channel_id, now, hours, req_seed);

                SQLite::Statement q(db_.get(), R"(
                    SELECT sp.item_type, sp.item_id, sp.block_id,
                           sp.wall_clock_start, sp.wall_clock_end, sp.status,
                           COALESCE(e.title, m.title, '') AS item_title,
                           COALESCE(s.title, '')           AS show_title,
                           COALESCE(e.show_id, '')         AS show_id,
                           COALESCE(e.season,  0)          AS season,
                           COALESCE(e.episode, 0)          AS ep_num,
                           COALESCE(e.duration_ms, m.duration_ms,
                                    (sp.wall_clock_end - sp.wall_clock_start) * 1000)
                               AS duration_ms
                    FROM scheduled_program sp
                    LEFT JOIN episode e ON sp.item_type='episode' AND sp.item_id=e.episode_id
                    LEFT JOIN show    s ON sp.item_type='episode' AND e.show_id=s.show_id
                    LEFT JOIN movie   m ON sp.item_type='movie'   AND sp.item_id=m.movie_id
                    WHERE sp.channel_id=?
                      AND sp.wall_clock_start >= ?
                      AND sp.wall_clock_start <  ?
                      AND sp.status != 'skipped'
                    ORDER BY sp.wall_clock_start
                )");
                q.bind(1, channel_id);
                q.bind(2, static_cast<int64_t>(now));
                q.bind(3, horizon);
                while (q.executeStep()) {
                    json j = {
                        {"item_type",           q.getColumn(0).getString()},
                        {"item_id",             q.getColumn(1).getString()},
                        {"block_id",            q.getColumn(2).isNull() ? "" : q.getColumn(2).getString()},
                        {"wall_clock_start_ms", q.getColumn(3).getInt64() * 1000},
                        {"wall_clock_end_ms",   q.getColumn(4).getInt64() * 1000},
                        {"status",              q.getColumn(5).getString()},
                        {"title",               q.getColumn(6).getString()},
                        {"duration_ms",         q.getColumn(11).getInt64()},
                    };
                    std::string show_title = q.getColumn(7).getString();
                    if (!show_title.empty()) {
                        j["show_title"]  = show_title;
                        j["show_id"]     = q.getColumn(8).getString();
                        j["season"]      = q.getColumn(9).getInt();
                        j["episode_num"] = q.getColumn(10).getInt();
                    }
                    arr.push_back(j);
                }
            } catch (...) {
                db_.get().exec("ROLLBACK TO SAVEPOINT preview_sp");
                db_.get().exec("RELEASE SAVEPOINT preview_sp");
                throw;
            }
            db_.get().exec("ROLLBACK TO SAVEPOINT preview_sp");
            db_.get().exec("RELEASE SAVEPOINT preview_sp");
        }

        ok(res, arr.dump());
      } catch (const std::exception& e) {
        logErr("GET /api/channels/epg/preview", e); err(res, 500, e.what());
      }
    });

    // ── EPG projection for a single channel (JSON, cache-backed) ────────────
    svr_.Get(R"(/api/channels/([^/]+)/epg)", [this](const Req& req, Res& res) {
      try {
        std::string channel_id = req.matches[1];
        int hours = 24;
        if (req.has_param("hours")) {
            try { hours = std::stoi(req.get_param_value("hours")); } catch (...) {}
        }
        hours = std::max(1, std::min(hours, 72));

        auto now = std::time(nullptr);
        materializer_.ensureScheduled(channel_id, now, hours);

        auto horizon = static_cast<int64_t>(now + hours * 3600LL);
        SQLite::Statement q(db_.get(), R"(
            SELECT sp.item_type, sp.item_id, sp.block_id,
                   sp.wall_clock_start, sp.wall_clock_end, sp.status,
                   COALESCE(e.title, m.title, '') AS item_title,
                   COALESCE(s.title, '')           AS show_title,
                   COALESCE(e.show_id, '')         AS show_id,
                   COALESCE(e.season,  0)          AS season,
                   COALESCE(e.episode, 0)          AS ep_num,
                   COALESCE(e.file_path, m.file_path, '') AS file_path,
                   COALESCE(e.duration_ms, m.duration_ms,
                            (sp.wall_clock_end - sp.wall_clock_start) * 1000) AS duration_ms
            FROM scheduled_program sp
            LEFT JOIN episode e ON sp.item_type='episode' AND sp.item_id=e.episode_id
            LEFT JOIN show    s ON sp.item_type='episode' AND e.show_id=s.show_id
            LEFT JOIN movie   m ON sp.item_type='movie'   AND sp.item_id=m.movie_id
            WHERE sp.channel_id=?
              AND sp.wall_clock_start >= ?
              AND sp.wall_clock_start <  ?
              AND sp.status != 'skipped'
            ORDER BY sp.wall_clock_start
        )");
        q.bind(1, channel_id);
        q.bind(2, static_cast<int64_t>(now));
        q.bind(3, horizon);

        json arr = json::array();
        while (q.executeStep()) {
            json j = {
                {"item_type",           q.getColumn(0).getString()},
                {"item_id",             q.getColumn(1).getString()},
                {"block_id",            q.getColumn(2).isNull() ? "" : q.getColumn(2).getString()},
                {"wall_clock_start_ms", q.getColumn(3).getInt64() * 1000},
                {"wall_clock_end_ms",   q.getColumn(4).getInt64() * 1000},
                {"status",              q.getColumn(5).getString()},
                {"title",               q.getColumn(6).getString()},
                {"file_path",           q.getColumn(11).getString()},
                {"duration_ms",         q.getColumn(12).getInt64()},
            };
            std::string show_title = q.getColumn(7).getString();
            if (!show_title.empty()) {
                j["show_title"]  = show_title;
                j["show_id"]     = q.getColumn(8).getString();
                j["season"]      = q.getColumn(9).getInt();
                j["episode_num"] = q.getColumn(10).getInt();
            }
            arr.push_back(j);
        }
        ok(res, arr.dump());
      } catch (const std::exception& e) {
        logErr("GET /api/channels/epg", e); err(res, 500, e.what());
      }
    });
}
