#include "Router.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include "log/LogBuffer.h"
#include "sync/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
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

} // namespace

void Router::ok(Res& res, const std::string& body) {
    res.set_content(body, "application/json");
}

void Router::err(Res& res, int status, const std::string& msg) {
    res.status = status;
    res.set_content(json{{"error", msg}}.dump(), "application/json");
}

// ---------------------------------------------------------------------------

Router::Router(httplib::Server& svr, Database& db, SyncManager& sync,
               ConfStore& conf, LogBuffer& logs)
    : svr_(svr), db_(db), sync_(sync), conf_(conf), logs_(logs)
{}

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
    registerContentRoutes();
    registerActivityRoutes();

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
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

void Router::registerChannelRoutes() {

    svr_.Get("/api/channels", [this](const Req&, Res& res) {
        SQLite::Statement q(db_.get(),
            "SELECT channel_id, name, number, timezone FROM channel ORDER BY number");
        json result = json::array();
        while (q.executeStep()) {
            result.push_back({
                {"channel_id", q.getColumn(0).getString()},
                {"name",       q.getColumn(1).getString()},
                {"number",     q.getColumn(2).getInt()},
                {"timezone",   q.getColumn(3).getString()},
            });
        }
        ok(res, result.dump());
    });

    svr_.Post("/api/channels", [this](const Req& req, Res& res) {
        try {
            auto b = json::parse(req.body);
            std::string name     = b.value("name", "");
            int         number   = b.value("number", 0);
            std::string timezone = b.value("timezone", "UTC");
            if (name.empty() || number == 0) {
                err(res, 400, "name and number required"); return;
            }
            std::string channel_id = generateId();
            SQLite::Statement s(db_.get(),
                "INSERT INTO channel (channel_id, name, number, timezone) VALUES (?,?,?,?)");
            s.bind(1, channel_id); s.bind(2, name);
            s.bind(3, number);     s.bind(4, timezone);
            s.exec();
            res.status = 201;
            ok(res, json{{"channel_id", channel_id}}.dump());
        } catch (const SQLite::Exception& e) {
            err(res, 409, e.what());
        } catch (const json::exception& e) {
            err(res, 400, e.what());
        }
    });

    svr_.Delete("/api/channels/:id", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement s(db_.get(), "DELETE FROM channel WHERE channel_id = ?");
        s.bind(1, id);
        s.exec();
        ok(res, json{{"deleted", id}}.dump());
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

    svr_.Get("/api/shows", [this](const Req& req, Res& res) {
        int         limit      = 50;
        int         offset     = 0;
        std::string library_id;
        if (req.has_param("limit"))      limit      = std::stoi(req.get_param_value("limit"));
        if (req.has_param("offset"))     offset     = std::stoi(req.get_param_value("offset"));
        if (req.has_param("library_id")) library_id = req.get_param_value("library_id");

        int  total = 0;
        json items = json::array();

        if (library_id.empty()) {
            SQLite::Statement cnt(db_.get(), "SELECT COUNT(*) FROM show");
            if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

            SQLite::Statement q(db_.get(), R"(
                SELECT s.show_id, s.title, s.content_rating,
                       COUNT(e.episode_id) AS episode_count
                FROM show s
                LEFT JOIN episode e ON e.show_id = s.show_id
                GROUP BY s.show_id ORDER BY s.title LIMIT ? OFFSET ?
            )");
            q.bind(1, limit); q.bind(2, offset);
            while (q.executeStep())
                items.push_back({{"show_id", q.getColumn(0).getString()},
                                 {"title",          q.getColumn(1).getString()},
                                 {"content_rating", q.getColumn(2).getString()},
                                 {"episode_count",  q.getColumn(3).getInt()}});
        } else {
            SQLite::Statement cnt(db_.get(), R"(
                SELECT COUNT(DISTINCT s.show_id) FROM show s
                JOIN source_mapping sm
                    ON sm.kairos_id = s.show_id AND sm.item_type = 'show'
                    AND sm.library_id = ?
            )");
            cnt.bind(1, library_id);
            if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

            SQLite::Statement q(db_.get(), R"(
                SELECT s.show_id, s.title, s.content_rating,
                       COUNT(e.episode_id) AS episode_count
                FROM show s
                JOIN source_mapping sm
                    ON sm.kairos_id = s.show_id AND sm.item_type = 'show'
                    AND sm.library_id = ?
                LEFT JOIN episode e ON e.show_id = s.show_id
                GROUP BY s.show_id ORDER BY s.title LIMIT ? OFFSET ?
            )");
            q.bind(1, library_id); q.bind(2, limit); q.bind(3, offset);
            while (q.executeStep())
                items.push_back({{"show_id",        q.getColumn(0).getString()},
                                 {"title",          q.getColumn(1).getString()},
                                 {"content_rating", q.getColumn(2).getString()},
                                 {"episode_count",  q.getColumn(3).getInt()}});
        }
        ok(res, json{{"items", items}, {"total", total}}.dump());
    });

    svr_.Get("/api/shows/:id/episodes", [this](const Req& req, Res& res) {
        auto id = req.path_params.at("id");
        SQLite::Statement q(db_.get(), R"(
            SELECT episode_id, season, episode, title, duration_ms, overview, air_date, thumb
            FROM episode WHERE show_id = ?
            ORDER BY season, episode
        )");
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

    svr_.Get("/api/movies", [this](const Req& req, Res& res) {
        int         limit      = 50;
        int         offset     = 0;
        std::string library_id;
        if (req.has_param("limit"))      limit      = std::stoi(req.get_param_value("limit"));
        if (req.has_param("offset"))     offset     = std::stoi(req.get_param_value("offset"));
        if (req.has_param("library_id")) library_id = req.get_param_value("library_id");

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
            SQLite::Statement cnt(db_.get(), "SELECT COUNT(*) FROM movie");
            if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

            SQLite::Statement q(db_.get(),
                "SELECT movie_id, title, content_rating, duration_ms, year "
                "FROM movie ORDER BY title LIMIT ? OFFSET ?");
            q.bind(1, limit); q.bind(2, offset);
            while (q.executeStep()) pushMovie(items, q);
        } else {
            SQLite::Statement cnt(db_.get(), R"(
                SELECT COUNT(DISTINCT m.movie_id) FROM movie m
                JOIN source_mapping sm
                    ON sm.kairos_id = m.movie_id AND sm.item_type = 'movie'
                    AND sm.library_id = ?
            )");
            cnt.bind(1, library_id);
            if (cnt.executeStep()) total = cnt.getColumn(0).getInt();

            SQLite::Statement q(db_.get(), R"(
                SELECT m.movie_id, m.title, m.content_rating, m.duration_ms, m.year
                FROM movie m
                JOIN source_mapping sm
                    ON sm.kairos_id = m.movie_id AND sm.item_type = 'movie'
                    AND sm.library_id = ?
                ORDER BY m.title LIMIT ? OFFSET ?
            )");
            q.bind(1, library_id); q.bind(2, limit); q.bind(3, offset);
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
