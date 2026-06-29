#include "ScraperService.h"
#include "scraper/ScraperManager.h"
#include "../RouteHelpers.h"
#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

static void ok(Res& res, const json& body) {
    res.set_content(body.dump(), "application/json");
}
static void err(Res& res, int status, const std::string& msg) {
    res.status = status;
    res.set_content(json{{"error", msg}}.dump(), "application/json");
}

ScraperService::ScraperService(ScraperManager& scraper) : scraper_(scraper) {}

void ScraperService::registerRoutes(httplib::Server& svr) {

    // GET /api/scrapers/config
    svr.Get("/api/scrapers/config", [this](const Req&, Res& res) {
        auto s = scraper_.getSettings();
        json out;
        out["match_threshold"] = s.match_threshold;
        out["configs"] = json::array();
        for (const auto& c : s.configs) {
            json cj;
            cj["source"]   = c.source;
            cj["api_key"]  = c.api_key;
            cj["language"] = c.language;
            cj["enabled"]  = c.enabled;
            if (c.source == "tvdb")  cj["pin"] = c.pin;
            if (c.source == "anidb") cj["note"] = "api_key is your registered AniDB client name";
            out["configs"].push_back(cj);
        }
        ok(res, out);
    });

    // PATCH /api/scrapers/config
    svr.Patch("/api/scrapers/config", [this](const Req& req, Res& res) {
        try {
            auto body = json::parse(req.body);
            ScraperSettings s = scraper_.getSettings();

            if (body.contains("match_threshold") && body["match_threshold"].is_number())
                s.match_threshold = body["match_threshold"].get<double>();

            if (body.contains("configs") && body["configs"].is_array()) {
                for (const auto& cj : body["configs"]) {
                    std::string src = cj.value("source", "");
                    for (auto& c : s.configs) {
                        if (c.source != src) continue;
                        if (cj.contains("api_key"))  c.api_key  = cj["api_key"].get<std::string>();
                        if (cj.contains("language")) c.language = cj["language"].get<std::string>();
                        if (cj.contains("enabled"))  c.enabled  = cj["enabled"].get<bool>();
                        if (src == "tvdb" && cj.contains("pin")) c.pin = cj["pin"].get<std::string>();
                    }
                }
            }
            scraper_.updateSettings(s);
            ok(res, json{{"ok", true}});
        } catch (const std::exception& e) {
            err(res, 400, e.what());
        }
    });

    // POST /api/scrapers/match
    svr.Post("/api/scrapers/match", [this](const Req& req, Res& res) {
        std::string target_id, item_type;
        try {
            if (!req.body.empty()) {
                auto body = json::parse(req.body);
                target_id = body.value("target_id", "");
                item_type = body.value("item_type", "");
            }
        } catch (...) {}

        if (scraper_.isMatching()) {
            res.status = 202;
            ok(res, json{{"status", "already_running"}});
            return;
        }
        scraper_.triggerMatch(target_id, item_type);
        res.status = 202;
        ok(res, json{{"status", "started"}});
    });

    // GET /api/scrapers/match/status
    svr.Get("/api/scrapers/match/status", [this](const Req&, Res& res) {
        ok(res, json{{"running", scraper_.isMatching()}});
    });

    // GET /api/scrapers/stats
    svr.Get("/api/scrapers/stats", [this](const Req&, Res& res) {
        auto s = scraper_.stats();
        ok(res, json{
            {"total",     s.total},
            {"matched",   s.matched},
            {"uncertain", s.uncertain},
            {"unmatched", s.unmatched},
            {"unscraped", s.unscraped},
        });
    });

    // GET /api/scrapers/queue
    svr.Get("/api/scrapers/queue", [this](const Req& req, Res& res) {
        std::string status_filter = "all";
        int limit = 48, offset = 0;
        if (req.has_param("status"))  status_filter = req.get_param_value("status");
        if (req.has_param("limit"))   { try { limit  = std::stoi(req.get_param_value("limit")); } catch (...) {} }
        if (req.has_param("offset"))  { try { offset = std::stoi(req.get_param_value("offset")); } catch (...) {} }

        auto items = scraper_.getQueue(status_filter, limit, offset);
        int total  = scraper_.queueTotal(status_filter);

        json arr = json::array();
        for (const auto& qi : items) {
            json qj;
            qj["kairos_id"]      = qi.kairos_id;
            qj["item_type"]      = qi.item_type;
            qj["title"]          = qi.title;
            if (qi.year > 0) qj["year"] = qi.year;
            qj["thumb"]          = qi.thumb;
            qj["source_id"]      = qi.source_id;
            qj["source_base_url"]= qi.source_base_url;
            qj["match_status"]   = qi.match_status;
            qj["match_score"]    = qi.match_score;
            qj["candidates"] = json::array();
            for (const auto& c : qi.candidates) {
                json cj;
                cj["candidate_id"] = c.candidate_id;
                cj["source"]       = c.source;
                cj["external_id"]  = c.external_id;
                cj["title"]        = c.title;
                if (c.year > 0) cj["year"] = c.year;
                cj["score"]        = c.score;
                cj["accepted"]     = (c.accepted == -1) ? json(nullptr) : json(c.accepted == 1);
                cj["poster_url"]   = c.poster_url;
                cj["overview"]     = c.overview;
                qj["candidates"].push_back(cj);
            }
            arr.push_back(qj);
        }
        ok(res, json{{"items", arr}, {"total", total}});
    });

    // POST /api/scrapers/queue/:kairos_id/manual-match
    svr.Post(R"(/api/scrapers/queue/([^/]+)/manual-match)", [this](const Req& req, Res& res) {
        std::string kairos_id = req.matches[1];
        try {
            auto body = json::parse(req.body);
            std::string item_type   = body.value("item_type",   "");
            std::string source      = body.value("source",      "");
            std::string external_id = body.value("external_id", "");
            std::string title       = body.value("title",       "");
            int year = (body.contains("year") && body["year"].is_number()) ? body["year"].get<int>() : 0;
            std::string poster_url  = body.value("poster_url",  "");
            std::string overview    = body.value("overview",    "");
            if (item_type.empty() || source.empty() || external_id.empty()) {
                err(res, 400, "item_type, source, and external_id are required");
                return;
            }
            if (scraper_.manualMatch(kairos_id, item_type, source, external_id, title, year, poster_url, overview))
                ok(res, json{{"ok", true}});
            else
                err(res, 404, "item not found");
        } catch (const std::exception& e) {
            err(res, 400, e.what());
        }
    });

    // POST /api/scrapers/queue/:id/accept
    svr.Post(R"(/api/scrapers/queue/([^/]+)/accept)", [this](const Req& req, Res& res) {
        std::string cid = req.matches[1];
        if (scraper_.acceptCandidate(cid))
            ok(res, json{{"ok", true}});
        else
            err(res, 404, "candidate not found");
    });

    // POST /api/scrapers/queue/:id/reject
    svr.Post(R"(/api/scrapers/queue/([^/]+)/reject)", [this](const Req& req, Res& res) {
        std::string cid = req.matches[1];
        if (scraper_.rejectCandidate(cid))
            ok(res, json{{"ok", true}});
        else
            err(res, 404, "candidate not found");
    });

    // GET /api/scrapers/search?q=...&type=show|movie
    svr.Get("/api/scrapers/search", [this](const Req& req, Res& res) {
        std::string q    = req.has_param("q")    ? req.get_param_value("q")    : "";
        std::string type = req.has_param("type") ? req.get_param_value("type") : "";
        if (q.empty()) { err(res, 400, "q is required"); return; }

        auto results = scraper_.search(q, type);
        json arr = json::array();
        for (const auto& r : results) {
            json rj;
            rj["source"]       = r.source;
            rj["external_id"]  = r.external_id;
            rj["title"]        = r.title;
            if (r.year > 0) rj["year"] = r.year;
            rj["overview"]     = r.overview;
            rj["poster_url"]   = r.poster_url.empty() ? ""
                                 : "/api/images/proxy?url=" + route::urlEncode(r.poster_url);
            rj["content_type"] = r.content_type;
            rj["in_library"]   = r.in_library;
            arr.push_back(rj);
        }
        ok(res, json{{"items", arr}});
    });
}
