#include "ActivityRouter.h"
#include "../log/LogBuffer.h"
#include "../stream/EncoderArgs.h" // hwAccelName
#include "../stream/SessionManager.h"
#include "../stream/VodSessionManager.h"
#include <filesystem>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

// VodSession has no human title (Router.cpp's /stream/vod/start handler
// fetches one from Kairos but never threads it into VodSession — adding
// that would mean changing VodSession::start()'s signature, which Router.cpp
// calls positionally, so it isn't a change confined to this router). The
// filename stem is a reasonable stand-in for a debugging/activity view.
std::string titleFromPath(const std::string& file_path) {
    if (file_path.empty()) return "";
    return std::filesystem::path(file_path).stem().string();
}

json channelSessionJson(const std::shared_ptr<ChannelSession>& s) {
    return {
        {"id",              s->channelId()},
        {"kind",            "channel"},
        {"title",           s->currentTitle()},
        {"file_path",       s->currentFilePath()},
        {"hw_accel",        hwAccelName(s->hwAccel())},
        {"decode_hw_accel", hwAccelName(s->decodeHwAccel())},
        {"started_at_ms",   s->sessionStartMs()},
    };
}

json vodSessionJson(const std::shared_ptr<VodSession>& s) {
    return {
        {"id",              s->sessionId()},
        {"kind",            "vod"},
        {"title",           titleFromPath(s->filePath())},
        {"file_path",       s->filePath()},
        {"hw_accel",        hwAccelName(s->hwAccel())},
        {"decode_hw_accel", hwAccelName(s->decodeHwAccel())},
        {"started_at_ms",   s->startedAtMs()},
        {"direct_play",     s->directPlay()},
    };
}

} // namespace

void registerActivityRoutes(httplib::Server& svr, SessionManager& sessions,
                             VodSessionManager& vodSessions, LogBuffer& logs) {
    svr.Get("/stream/activity/sessions", [&sessions, &vodSessions](
            const httplib::Request&, httplib::Response& res) {
        json out = json::array();
        for (auto& s : sessions.listActive())    out.push_back(channelSessionJson(s));
        for (auto& s : vodSessions.listActive()) out.push_back(vodSessionJson(s));
        res.set_content(out.dump(), "application/json");
    });

    // v1 approach: the shared LogBuffer isn't partitioned per session, so
    // this filters the recent-lines window by the "[session:<id>"/
    // "[vod:<id>"/"[preview:<id>" prefix every session's own logging already
    // uses (see ChannelSession/VodSession/PreviewSession's std::cerr/cout
    // calls) rather than a live per-session SSE stream — Hermes's proxyRequest
    // fully buffers the response before forwarding it, so it can't relay an
    // SSE stream anyway (a stream that never completes would just hang the
    // proxy). Polling this endpoint is the pattern that actually works
    // through that proxy.
    svr.Get(R"(/stream/activity/sessions/([^/]+)/logs)", [&logs](
            const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        int lines = 500;
        if (req.has_param("lines")) {
            try { lines = std::stoi(req.get_param_value("lines")); } catch (...) {}
        }

        const std::string tagChannel = "[session:" + id;
        const std::string tagVod     = "[vod:"     + id;
        const std::string tagPreview = "[preview:" + id;

        // Pull a generously large recent window (the shared buffer holds up
        // to LogBuffer::kMax=2000 lines total across every session) then
        // filter down to this session's own lines.
        auto [recent, seq] = logs.recent(LogBuffer::kMax);
        std::vector<std::string> matched;
        for (auto& line : recent) {
            if (line.find(tagChannel) != std::string::npos ||
                line.find(tagVod)     != std::string::npos ||
                line.find(tagPreview) != std::string::npos)
                matched.push_back(line);
        }
        if (static_cast<int>(matched.size()) > lines)
            matched.erase(matched.begin(), matched.end() - lines);

        res.set_content(json(matched).dump(), "application/json");
    });
}
