#include "Router.h"
#include <nlohmann/json.hpp>
#include <chrono>
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
    std::cout << "[hermes] stream request: channel=" << channel_id << "\n";

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

static std::string urlEncodeValue(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

static std::string urlDecodeValue(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = s[i + 1], lo = s[i + 2];
            if (std::isxdigit(hi) && std::isxdigit(lo)) {
                if (hi >= '0' && hi <= '9') hi -= '0';
                else if (hi >= 'a' && hi <= 'f') hi -= 'a' - 10;
                else if (hi >= 'A' && hi <= 'F') hi -= 'A' - 10;
                if (lo >= '0' && lo <= '9') lo -= '0';
                else if (lo >= 'a' && lo <= 'f') lo -= 'a' - 10;
                else if (lo >= 'A' && lo <= 'F') lo -= 'A' - 10;
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Build query string from httplib params map, re-encoding values.
static std::string buildQuery(const httplib::Params& params) {
    std::string q;
    for (auto& [k, v] : params)
        q += (q.empty() ? "" : "&") + k + "=" + urlEncodeValue(v);
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
    for (const char* h : {"Authorization", "Cookie", "Accept", "Accept-Language", "X-Pantheon-Surface"}) {
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
    for (const char* h : {"Authorization", "Cookie", "Accept", "Accept-Language", "X-Pantheon-Surface"}) {
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
    auto loc = r->get_header_value("Location");
    if (!loc.empty()) res.set_header("Location", loc);
    // Needed for a Chromecast custom receiver's player pipeline (a JS-level
    // fetch from whatever origin the receiver is hosted at — github.io or a
    // self-hosted domain) to read manifests/segments cross-origin if it ever
    // falls back off native passthrough. Same wide-open posture proxyStream
    // and the SSE log route already use elsewhere in this file.
    res.set_header("Access-Control-Allow-Origin", "*");
    auto resp_ct = r->get_header_value("Content-Type");
    if (!r->body.empty())
        res.set_content(r->body, resp_ct.empty() ? "application/octet-stream" : resp_ct);
}

void registerRoutes(httplib::Server& svr, BroadcasterManager& broadcasters,
                    KairosClient& kairos, LogBuffer& logs, const Config& cfg) {

    // ── Health ────────────────────────────────────────────────────────────────
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            json{{"status","ok"},{"service","hermes"}}.dump(),
            "application/json");
    });

    // ── Log stream (SSE) — Hermes's own log buffer ───────────────────────────
    svr.Get("/api/logs/stream", [&logs](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control",     "no-cache");
        res.set_header("Connection",        "keep-alive");
        res.set_header("X-Accel-Buffering", "no");
        res.set_header("Access-Control-Allow-Origin", "*");

        res.set_chunked_content_provider("text/event-stream",
            [&logs, cur_seq = uint64_t{0}, sent_init = false]
            (size_t, httplib::DataSink& sink) mutable -> bool {

                if (!sent_init) {
                    sent_init = true;
                    auto [lines, seq] = logs.recent(200);
                    cur_seq = seq;
                    for (const auto& line : lines) {
                        std::string ev = "data:" + line + "\n\n";
                        if (!sink.write(ev.data(), ev.size())) return false;
                    }
                    return true;
                }

                auto [new_lines, new_seq] =
                    logs.waitAfter(cur_seq, std::chrono::milliseconds{25'000});

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

    // ── Image proxy (for CORS/PNA bypass on external art like Plex) ──────────
    svr.Get("/api/images/proxy", [&cfg](const httplib::Request& req, httplib::Response& res) {
        auto url = req.get_param_value("url");
        auto token = req.get_param_value("token");
        if (url.empty()) {
            res.status = 400;
            return;
        }

        // Validate token via Kairos before fetching external resource
        httplib::Client authCli(cfg.kairos_url);
        authCli.set_connection_timeout(5);
        authCli.set_read_timeout(5);
        auto authRes = authCli.Get("/api/auth/me", httplib::Headers{{"Authorization", "Bearer " + token}});
        if (!authRes || authRes->status != 200) {
            res.status = 401;
            return;
        }

        // Split URL into base and path
        size_t proto = url.find("://");
        if (proto == std::string::npos) { res.status = 400; return; }
        size_t host_start = proto + 3;
        size_t path_start = url.find('/', host_start);
        std::string base = url.substr(0, path_start);
        std::string path = (path_start == std::string::npos) ? "/" : url.substr(path_start);

        httplib::Client cli(base);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(10);
        cli.set_follow_location(true);
        auto r = cli.Get(path);

        if (!r || r->status == 0) {
            res.status = 502;
            return;
        }

        res.status = r->status;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "public, max-age=86400"); // 24h
        auto resp_ct = r->get_header_value("Content-Type");
        res.set_content(r->body, resp_ct.empty() ? "image/jpeg" : resp_ct);
    });

    // ── MPEG-TS live stream ───────────────────────────────────────────────────
    // DVR clients (Plex, Jellyfin) send Range headers even for live streams.
    // httplib's range_error check fires after the handler and returns 416
    // whenever the chunked content provider has no content-length (always for
    // live streams). The Request object is non-const internally — clearing
    // ranges here prevents the spurious 416 while keeping chunked encoding.
    svr.Get(R"(/stream/channels/([^/.]+)$)", [&broadcasters](
            const httplib::Request& req, httplib::Response& res) {
        const_cast<httplib::Request&>(req).ranges.clear();
        handleStream(req.matches[1], broadcasters, res);
    });

    svr.Get(R"(/stream/channels/([^/]+)\.ts$)", [&broadcasters](
            const httplib::Request& req, httplib::Response& res) {
        const_cast<httplib::Request&>(req).ranges.clear();
        handleStream(req.matches[1], broadcasters, res);
    });

    // ── HLS / VOD proxy — 1:1 pass-through to Hephaestus, no fan-out ──────────
    // Unlike /stream/channels/:id (MPEG-TS, shared ClientSink fan-out via
    // handleStream above), HLS manifests/segments/subtitles are finite,
    // complete HTTP responses that Hephaestus already serves as static
    // files — proxyRequest's buffered pass-through is the right fit, same as
    // the Kairos API proxy below. Registered before the Hades catch-all.
    auto hephaestusProxy = [&cfg](const httplib::Request& req, httplib::Response& res) {
        proxyRequest(cfg.hephaestus_url, req, res);
    };
    svr.Get(R"(/stream/hls/channels/.*)", hephaestusProxy);
    svr.Get(R"(/stream/vod/.*)",     hephaestusProxy);
    svr.Get(R"(/stream/preview/.*)", hephaestusProxy);

    // POST /stream/vod|preview/... (start/switch/stop) is the one place on
    // this router that would otherwise let anyone on the network stream the
    // private library with no login — unlike /stream/channels/:id (open on
    // purpose, for HDHomeRun/DVR compatibility) there's no third-party-client
    // reason for this to be unauthenticated. Hermes has no session store of
    // its own, so it asks Kairos to validate the caller's token first.
    auto authedHephaestusProxy = [&cfg](const httplib::Request& req, httplib::Response& res) {
        auto auth = req.get_header_value("Authorization");
        httplib::Result r;
        if (!auth.empty()) {
            httplib::Client cli(cfg.kairos_url);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(5);
            r = cli.Get("/api/auth/me", httplib::Headers{{"Authorization", auth}});
        }
        if (auth.empty() || !r || r->status != 200) {
            res.status = 401;
            res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
            return;
        }

        // Parental controls — only "start"/"switch" calls carry enough info
        // (content_type+content_id for VOD, channel_id for preview) to check;
        // stop/activity calls operate on an already-authorized session and
        // are left alone. This is the one already-authenticated playback
        // boundary for VOD/preview, so restriction is enforced here as a
        // real security check, not just hidden from browse/list views.
        // Fails open on an infrastructure error (Kairos already had to be
        // reachable for the auth check above to have succeeded at all) and
        // closed only on an explicit "not allowed" response.
        bool is_start_or_switch = req.path.ends_with("/start") || req.path.ends_with("/switch");
        if (is_start_or_switch) {
            try {
                auto body = json::parse(req.body);
                httplib::Client kairos(cfg.kairos_url);
                kairos.set_connection_timeout(5);
                kairos.set_read_timeout(5);
                httplib::Result check;
                if (req.path.starts_with("/stream/vod/") &&
                    body.contains("content_type") && body.contains("content_id")) {
                    std::string path = "/api/content/" + body["content_type"].get<std::string>()
                                      + "/" + body["content_id"].get<std::string>() + "/access-check";
                    check = kairos.Get(path, httplib::Headers{{"Authorization", auth}});
                } else if (req.path.starts_with("/stream/preview/") && body.contains("channel_id")) {
                    std::string path = "/api/channels/" + body["channel_id"].get<std::string>() + "/access-check";
                    check = kairos.Get(path, httplib::Headers{{"Authorization", auth}});
                }
                if (check && check->status == 200) {
                    auto cj = json::parse(check->body);
                    if (!cj.value("allowed", true)) {
                        res.status = 403;
                        res.set_content(json{{"error", "Restricted"}}.dump(), "application/json");
                        return;
                    }
                }
            } catch (...) { /* malformed body or unreachable check — fail open, per above */ }
        }

        proxyRequest(cfg.hephaestus_url, req, res);
    };
    svr.Post(R"(/stream/vod/.*)",     authedHephaestusProxy);
    svr.Post(R"(/stream/preview/.*)", authedHephaestusProxy);

    // GET /stream/activity/... (Hades' Activity page "Now Playing" panel) —
    // authenticated for the same reason as the POST routes above: unlike a
    // segment/manifest fetch for a stream ID the caller already knows, this
    // exposes what *other* users are currently watching plus internal file
    // paths and server log lines, which is a step beyond what an
    // unauthenticated GET route should hand out.
    svr.Get(R"(/stream/activity/.*)", authedHephaestusProxy);

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

    // ── M3U / XMLTV aliases ───────────────────────────────────────────────────
    // Common alternate paths used by DVR clients — must come before /api/.*
    // Uses a dedicated client with a longer read timeout than proxyRequest because
    // XMLTV generation can be slow on a cold cache (ensureScheduled for every channel).
    auto xmltvAlias = [&cfg](const httplib::Request& req, httplib::Response& res) {
        int hours = 24;
        auto it = req.params.find("hours");
        if (it != req.params.end()) {
            try { hours = std::stoi(it->second); } catch (...) {}
        }
        httplib::Client cli(cfg.kairos_url);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(60);
        auto r = cli.Get("/epg.xml?hours=" + std::to_string(hours));
        if (!r || r->status == 0) { res.status = 502; return; }
        res.status = r->status;
        res.set_content(r->body, "application/xml");
    };
    svr.Get("/epg.xml",       xmltvAlias);
    svr.Get("/api/epg.xml",   xmltvAlias);
    svr.Get("/api/xmltv.xml", xmltvAlias);

    svr.Get("/api/channels.m3u", [&kairos](const httplib::Request& req, httplib::Response& res) {
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
    svr.Get("/api/channels.xml", [&cfg](const httplib::Request& req, httplib::Response& res) {
        int hours = 24;
        auto it = req.params.find("hours");
        if (it != req.params.end()) {
            try { hours = std::stoi(it->second); } catch (...) {}
        }
        httplib::Client cli(cfg.kairos_url);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(30);
        auto r = cli.Get("/epg.xml?hours=" + std::to_string(hours));
        if (!r || r->status == 0) { res.status = 502; return; }
        res.status = r->status;
        res.set_content(r->body, "application/xml");
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
