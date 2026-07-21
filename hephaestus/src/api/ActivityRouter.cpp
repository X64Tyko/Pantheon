#include "ActivityRouter.h"
#include "log/LogBuffer.h"
#include "../stream/EncoderArgs.h" // hwAccelName
#include "../stream/GpuMetrics.h"
#include "../stream/SessionManager.h"
#include "../stream/VodSessionManager.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <fstream>
#include <unistd.h>

// MetricsGatherer inline for shared logic across components without complex relative headers in Docker
struct ProcessMetrics {
    double cpu_usage = 0.0;
    long ram_bytes = 0;
};

class MetricsGatherer {
public:
    static ProcessMetrics getProcessMetrics() {
        ProcessMetrics m;
        std::ifstream statm("/proc/self/statm");
        long rss_pages = 0;
        if (statm >> rss_pages >> rss_pages) {
            m.ram_bytes = rss_pages * sysconf(_SC_PAGESIZE);
        }
        static long last_utime = 0, last_stime = 0, last_total_time = 0;
        std::ifstream stat("/proc/self/stat");
        std::string dummy;
        for (int i = 0; i < 13; ++i) stat >> dummy;
        long utime, stime;
        stat >> utime >> stime;
        std::ifstream uptime("/proc/stat");
        std::string cpu;
        uptime >> cpu;
        long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
        uptime >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;
        long total_time = user + nice + system + idle + iowait + irq + softirq + steal;
        if (last_total_time > 0) {
            long total_delta = total_time - last_total_time;
            long proc_delta = (utime + stime) - (last_utime + last_stime);
            if (total_delta > 0) m.cpu_usage = 100.0 * proc_delta / total_delta;
        }
        last_utime = utime; last_stime = stime; last_total_time = total_time;
        return m;
    }
};

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
        // client_count is exact (native MPEG-TS/DVR clients); hls_viewer_active
        // is a presence signal only, not a count — see ChannelSession's own
        // doc comments. A channel can be watched by several people at once,
        // unlike a VOD session (always exactly one viewer), so this is what
        // lets the activity view's viewer count actually reflect that instead
        // of undercounting every channel as "1".
        {"client_count",     s->clientCount()},
        {"hls_viewer_active", s->hlsViewerActive()},
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
                             VodSessionManager& vodSessions, LogBuffer& logs,
                             HwAccel gpu_backend, const std::string& vaapi_device) {
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

    svr.Get("/stream/activity/metrics", [gpu_backend, vaapi_device](const httplib::Request&, httplib::Response& res) {
        auto pm = MetricsGatherer::getProcessMetrics();
        json j = {
            {"cpu_usage", pm.cpu_usage},
            {"ram_bytes", pm.ram_bytes}
        };
        // Absent entirely when gpu_backend == HwAccel::none or the query
        // itself fails — the Activity page draws nothing in either case
        // rather than an empty/zeroed GPU section (real feedback: "not
        // drawn at all" when no hw-accel is available, not a blank card).
        if (auto gpu = queryGpuMetrics(gpu_backend, vaapi_device)) {
            json j_gpu = {
                {"backend",       gpu->backend},
                {"gpu_util_pct",  gpu->gpu_util_pct},
                {"mem_used_mb",   gpu->mem_used_mb},
                {"mem_total_mb",  gpu->mem_total_mb},
            };
            if (gpu->name)             j_gpu["name"]               = *gpu->name;
            if (gpu->encoder_util_pct) j_gpu["encoder_util_pct"]   = *gpu->encoder_util_pct;
            if (gpu->decoder_util_pct) j_gpu["decoder_util_pct"]   = *gpu->decoder_util_pct;
            if (gpu->temp_c)           j_gpu["temp_c"]             = *gpu->temp_c;
            j["gpu"] = j_gpu;
        }
        res.set_content(j.dump(), "application/json");
    });
}
