#include "ClientCapabilitiesRouter.h"
#include "../stream/ClientCapabilities.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

std::string extractBearerToken(const httplib::Request& req) {
    auto h = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (h.rfind(prefix, 0) != 0) return "";
    return h.substr(prefix.size());
}

// First 8 chars only — enough to distinguish clients across the handful of
// log lines a debugging session generates, without writing a real bearer
// credential to disk/console logs.
static std::string shortToken(const std::string& token) {
    return token.size() <= 8 ? token : token.substr(0, 8) + "...";
}

static std::string joinCodecs(const std::set<std::string>& codecs) {
    if (codecs.empty()) return "(none)";
    std::ostringstream ss;
    bool first = true;
    for (auto& c : codecs) { if (!first) ss << ","; ss << c; first = false; }
    return ss.str();
}

void registerClientCapabilitiesRoutes(httplib::Server& svr, ClientCapabilityCache& cache) {
    svr.Post("/stream/client-capabilities", [&cache](const httplib::Request& req, httplib::Response& res) {
        auto token = extractBearerToken(req);
        if (token.empty()) { res.status = 401; return; }

        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400; res.set_content(json{{"error", "invalid JSON"}}.dump(), "application/json"); return;
        }

        ClientCapabilities caps;
        for (auto& c : body.value("video_codecs", json::array()))
            if (c.is_string()) caps.video_codecs.insert(c.get<std::string>());
        for (auto& c : body.value("audio_codecs", json::array()))
            if (c.is_string()) caps.audio_codecs.insert(c.get<std::string>());

        std::cerr << "[client-caps] declared for token=" << shortToken(token)
                  << " video=[" << joinCodecs(caps.video_codecs) << "]"
                  << " audio=[" << joinCodecs(caps.audio_codecs) << "]\n";

        cache.declare(token, std::move(caps));
        res.status = 204;
    });

    // No error surfaced for a missing/already-absent token — logging out
    // twice, or logging out a token that was never declared in the first
    // place (e.g. this Hephaestus instance restarted since), should both
    // just quietly succeed from the client's point of view.
    svr.Delete("/stream/client-capabilities", [&cache](const httplib::Request& req, httplib::Response&) {
        auto token = extractBearerToken(req);
        if (!token.empty()) cache.forget(token);
    });
}
