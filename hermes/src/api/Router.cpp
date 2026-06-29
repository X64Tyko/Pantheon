#include "Router.h"
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

static std::string baseUrl(const httplib::Request& req) {
    auto host = req.get_header_value("Host");
    if (host.empty()) host = "localhost:8000";
    return "http://" + host;
}

static void handleStream(const std::string& channel_id,
                          BroadcasterManager& broadcasters,
                          httplib::Response& res) {
    auto bc = broadcasters.getOrCreate(channel_id);
    auto sink = bc->addClient();

    res.set_chunked_content_provider(
        "video/mp2t",
        [sink](size_t, httplib::DataSink& data_sink) -> bool {
            std::unique_lock<std::mutex> lock(sink->mtx);
            sink->cv.wait(lock, [&] {
                return !sink->queue.empty() || sink->done.load();
            });

            if (sink->queue.empty()) {
                data_sink.done();
                return false;
            }

            auto chunk = std::move(sink->queue.front());
            sink->queue.pop_front();
            lock.unlock();

            return data_sink.write(
                reinterpret_cast<const char*>(chunk.data()),
                chunk.size());
        },
        [sink, bc](bool) {
            bc->removeClient(sink);
        }
    );
}

// Build query string from httplib params map.
static std::string buildQuery(const httplib::Params& params) {
    std::string q;
    for (auto& [k, v] : params)
        q += (q.empty() ? "" : "&") + k + "=" + v;
    return q;
}

// Proxy a long-lived streaming response (SSE, chunked) from an upstream service.
// Unlike proxyRequest this never buffers — it pipes bytes to the client as they arrive.
static void proxyStream(const std::string& upstream_base,
                         const httplib::Request& req,
                         httplib::Response& res) {
    std::string path = req.path;
    auto q = buildQuery(req.params);
    if (!q.empty()) path += "?" + q;

    httplib::Headers fwd;
    for (const char* h : {"Authorization", "Cookie", "Accept", "Accept-Language"}) {
        auto v = req.get_header_value(h);
        if (!v.empty()) fwd.emplace(h, v);
    }

    res.set_header("Cache-Control",     "no-cache");
    res.set_header("Connection",        "keep-alive");
    res.set_header("X-Accel-Buffering", "no");
    res.set_header("Access-Control-Allow-Origin", "*");

    res.set_chunked_content_provider("text/event-stream",
        [upstream_base, path, fwd](size_t, httplib::DataSink& sink) -> bool {
            httplib::Client cli(upstream_base);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(60);

            cli.Get(path, fwd,
                [](const httplib::Response&) -> bool { return true; },
                [&sink](const char* data, size_t len) -> bool {
                    return sink.is_writable() && sink.write(data, len);
                });

            if (sink.is_writable()) sink.done();
            return false;
        });
}

// Forward any HTTP request to an upstream service verbatim.
static void proxyRequest(const std::string& upstream_base,
                          const httplib::Request& req,
                          httplib::Response& res) {
    httplib::Client cli(upstream_base);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(30);

    std::string path = req.path;
    auto q = buildQuery(req.params);
    if (!q.empty()) path += "?" + q;

    auto ct = req.get_header_value("Content-Type");

    // Forward headers that upstreams need for auth and content negotiation.
    httplib::Headers fwd;
    for (const char* h : {"Authorization", "Cookie", "Accept", "Accept-Language"}) {
        auto v = req.get_header_value(h);
        if (!v.empty()) fwd.emplace(h, v);
    }

    httplib::Result r;
    if      (req.method == "GET")    r = cli.Get(path, fwd);
    else if (req.method == "POST")   r = cli.Post(path, fwd, req.body, ct.c_str());
    else if (req.method == "PUT")    r = cli.Put(path, fwd, req.body, ct.c_str());
    else if (req.method == "DELETE") r = cli.Delete(path, fwd, req.body, ct.c_str());
    else if (req.method == "PATCH")  r = cli.Patch(path, fwd, req.body, ct.c_str());
    else { res.status = 405; return; }

    if (!r || r->status == 0) {
        res.status = 502;
        res.set_content(json{{"error", "upstream unavailable"}}.dump(), "application/json");
        return;
    }
    res.status = r->status;
    auto resp_ct = r->get_header_value("Content-Type");
    res.set_content(r->body, resp_ct.empty() ? "application/octet-stream" : resp_ct);
}

void registerRoutes(httplib::Server& svr, BroadcasterManager& broadcasters,
                    KairosClient& kairos, const Config& cfg) {

    // ── Health ────────────────────────────────────────────────────────────────
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            json{{"status","ok"},{"service","hermes"}}.dump(),
            "application/json");
    });

    // ── Log stream (SSE) — proxied from Kairos ───────────────────────────────
    svr.Get("/api/logs/stream", [&cfg](const httplib::Request& req, httplib::Response& res) {
        proxyStream(cfg.kairos_url, req, res);
    });

    // ── HDHomeRun device emulation ────────────────────────────────────────────
    svr.Get("/discover.json", [&cfg](const httplib::Request& req, httplib::Response& res) {
        auto base = baseUrl(req);
        res.set_content(json{
            {"FriendlyName",   cfg.hdhr_friendly},
            {"Manufacturer",   "Silicondust"},
            {"ManufacturerURL","https://github.com/X64Tyko/Pantheon"},
            {"ModelNumber",    "HDTC-2US"},
            {"FirmwareName",   "hdhomeruntc_atsc"},
            {"TunerCount",     cfg.hdhr_tuner_count},
            {"FirmwareVersion","20170930"},
            {"DeviceID",       cfg.hdhr_device_id},
            {"DeviceAuth",     ""},
            {"BaseURL",        base},
            {"LineupURL",      base + "/lineup.json"},
        }.dump(), "application/json");
    });

    svr.Get("/device.xml", [&cfg](const httplib::Request& req, httplib::Response& res) {
        auto base = baseUrl(req);
        std::string xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
            "  <URLBase>" + base + "</URLBase>\n"
            "  <specVersion><major>1</major><minor>0</minor></specVersion>\n"
            "  <device>\n"
            "    <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>\n"
            "    <friendlyName>" + cfg.hdhr_friendly + "</friendlyName>\n"
            "    <manufacturer>Silicondust</manufacturer>\n"
            "    <modelName>HDTC-2US</modelName>\n"
            "    <modelNumber>HDTC-2US</modelNumber>\n"
            "    <serialNumber/>\n"
            "    <UDN>uuid:" + cfg.hdhr_device_id + "</UDN>\n"
            "  </device>\n"
            "</root>\n";
        res.set_content(xml, "application/xml");
    });

    svr.Get("/lineup_status.json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{
            {"ScanInProgress", 0},
            {"ScanPossible",   1},
            {"Source",         "Cable"},
            {"SourceList",     json::array({"Cable"})},
        }.dump(), "application/json");
    });

    svr.Get("/lineup.json", [&kairos](const httplib::Request& req, httplib::Response& res) {
        auto base = baseUrl(req);
        auto channels = kairos.getChannels();
        json lineup = json::array();
        for (auto& ch : channels) {
            lineup.push_back({
                {"GuideNumber", std::to_string(ch.number)},
                {"GuideName",   ch.name},
                {"URL",         base + "/stream/channels/" + ch.channel_id + ".ts"},
            });
        }
        res.set_content(lineup.dump(), "application/json");
    });

    // ── MPEG-TS live stream ───────────────────────────────────────────────────
    svr.Get(R"(/stream/channels/([^/.]+)$)", [&broadcasters](
            const httplib::Request& req, httplib::Response& res) {
        handleStream(req.matches[1], broadcasters, res);
    });

    svr.Get(R"(/stream/channels/([^/]+)\.ts$)", [&broadcasters](
            const httplib::Request& req, httplib::Response& res) {
        handleStream(req.matches[1], broadcasters, res);
    });

    // ── M3U playlist ──────────────────────────────────────────────────────────
    svr.Get("/playlist.m3u", [&kairos](const httplib::Request& req, httplib::Response& res) {
        auto base = baseUrl(req);
        auto channels = kairos.getChannels();
        std::string m3u = "#EXTM3U\n";
        for (auto& ch : channels) {
            m3u += "#EXTINF:-1"
                   " tvg-id=\""         + ch.channel_id + "\""
                   " tvg-name=\""       + ch.name       + "\""
                   " channel-number=\"" + std::to_string(ch.number) + "\""
                   "," + ch.name + "\n"
                   + base + "/stream/channels/" + ch.channel_id + "\n";
        }
        res.set_content(m3u, "application/x-mpegurl");
    });

    svr.Get("/epg.xml", [&cfg](const httplib::Request& req, httplib::Response& res) {
        proxyRequest(cfg.kairos_url, req, res);
    });

    // ── Kairos API proxy (all methods) ────────────────────────────────────────
    // Registered before the Hades catch-all so /api/* routes never reach nginx.
    auto kairosProxy = [&cfg](const httplib::Request& req, httplib::Response& res) {
        proxyRequest(cfg.kairos_url, req, res);
    };
    svr.Get(R"(/api/.*)",    kairosProxy);
    svr.Post(R"(/api/.*)",   kairosProxy);
    svr.Put(R"(/api/.*)",    kairosProxy);
    svr.Delete(R"(/api/.*)", kairosProxy);
    svr.Patch(R"(/api/.*)",  kairosProxy);

    // ── Hades UI catch-all ────────────────────────────────────────────────────
    // Everything that didn't match above is the SPA. nginx's try_files handles
    // SPA routing server-side so deep links and refreshes work.
    auto hadesProxy = [&cfg](const httplib::Request& req, httplib::Response& res) {
        proxyRequest(cfg.hades_url, req, res);
    };
    svr.Get(".*",  hadesProxy);
    svr.Post(".*", hadesProxy);
}
