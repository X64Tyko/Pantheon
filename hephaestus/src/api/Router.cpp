#include "Router.h"
#include "../stream/MediaProbe.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using json = nlohmann::json;

// Polls briefly for a file ffmpeg is expected to produce shortly after a
// session starts (first HLS segment/playlist write). Avoids serving a 503 on
// the very first request just because ffmpeg hasn't flushed its first
// segment yet.
//
// 10s, not a smaller "should be plenty" number: cold start on a live channel
// stacks a probeMedia() ffprobe call, process spawn/codec init, and (since
// the live encode is -re-paced) a real wall-clock wait for the first
// hls_time-second segment to close — measured routinely landing at 4-5s even
// with hls_time=2. VOD adds its own probe-before-spawn cost too. A too-tight
// budget here doesn't fail gracefully — the client (hls.js) sees a 503,
// burns its own retry budget, and surfaces a fatal "stream stopped
// responding" with nothing useful in the server logs, since nothing here
// actually errored.
static bool waitForFile(const std::string& path, int maxWaitMs = 10000) {
    for (int waited = 0; waited < maxWaitMs; waited += 100) {
        if (std::filesystem::exists(path)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return std::filesystem::exists(path);
}

static void serveHlsFile(const std::string& path, const std::string& content_type,
                          httplib::Response& res) {
    if (!std::filesystem::exists(path)) {
        res.status = 404;
        res.set_content(json{{"error", "not found"}}.dump(), "application/json");
        return;
    }
    res.set_file_content(path, content_type);
}

// Extracts "http://host:port" from a request, falling back to the Host header.
static std::string baseUrl(const httplib::Request& req) {
    auto host = req.get_header_value("Host");
    if (host.empty()) host = "localhost:8082";
    return "http://" + host;
}

// Shared stream handler: looks up / creates a session and fans the client in.
static void handleStream(const std::string& channel_id,
                          SessionManager& sessions,
                          httplib::Response& res) {
    auto session = sessions.getOrCreate(channel_id);
    if (!session) {
        res.status = 503;
        res.set_content(
            json{{"error", "channel unavailable"}, {"channel_id", channel_id}}.dump(),
            "application/json");
        return;
    }

    auto sink = std::make_shared<ClientSink>();
    session->addClient(sink);

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
        [sink, session](bool) {
            session->removeClient(sink);
        }
    );
}

void registerRoutes(httplib::Server& svr, SessionManager& sessions, VodSessionManager& vodSessions,
                    PreviewSessionManager& previewSessions,
                    KairosClient& kairos, LogBuffer& logs, const Config& cfg) {

    // ── Health ────────────────────────────────────────────────────────────────
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            json{{"status", "ok"}, {"service", "hephaestus"}}.dump(),
            "application/json");
    });

    // ── Log stream (SSE — same contract as Kairos /api/logs/stream) ───────────
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
    // Plex, Emby, and Jellyfin all speak the HDHomeRun HTTP API for live TV
    // discovery. Add the device manually in the DVR settings by pointing at
    // http://<hephaestus-host>:<port>. No SSDP required for manual entry.

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

    // lineup.json: one entry per Kairos channel, stream URL points back here.
    // Uses the .ts suffix so Plex doesn't try to negotiate HLS.
    svr.Get("/lineup.json", [&kairos, &sessions](
            const httplib::Request& req, httplib::Response& res) {
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

        if (lineup.empty()) {
            lineup.push_back({
                {"GuideNumber", "1"},
                {"GuideName",   "No channels — check Kairos"},
                {"URL",         base + "/health"},
            });
        }

        res.set_content(lineup.dump(), "application/json");
    });

    // ── MPEG-TS stream ────────────────────────────────────────────────────────
    // Plain UUID form — used by M3U players and direct clients.
    svr.Get(R"(/stream/channels/([^/.]+)$)", [&sessions](
            const httplib::Request& req, httplib::Response& res) {
        handleStream(req.matches[1], sessions, res);
    });

    // .ts suffix form — used by HDHomeRun lineup.json and Plex.
    svr.Get(R"(/stream/channels/([^/]+)\.ts$)", [&sessions](
            const httplib::Request& req, httplib::Response& res) {
        handleStream(req.matches[1], sessions, res);
    });

    // ── M3U playlist ──────────────────────────────────────────────────────────
    svr.Get("/playlist.m3u", [&kairos](const httplib::Request& req, httplib::Response& res) {
        auto base = baseUrl(req);
        auto channels = kairos.getChannels();
        std::string m3u = "#EXTM3U\n";
        for (auto& ch : channels) {
            m3u += "#EXTINF:-1"
                   " tvg-id=\""      + ch.channel_id + "\""
                   " tvg-name=\""    + ch.name       + "\""
                   " channel-number=\"" + std::to_string(ch.number) + "\""
                   "," + ch.name + "\n"
                   + base + "/stream/channels/" + ch.channel_id + "\n";
        }
        res.set_content(m3u, "application/x-mpegurl");
    });

    // ── Internal channel list (proxied from Kairos) ───────────────────────────
    svr.Get("/api/channels", [&kairos](const httplib::Request&, httplib::Response& res) {
        auto channels = kairos.getChannels();
        json arr = json::array();
        for (auto& ch : channels)
            arr.push_back({{"channel_id", ch.channel_id},
                           {"name",       ch.name},
                           {"number",     ch.number}});
        res.set_content(arr.dump(), "application/json");
    });

    // ── Live channel HLS (web player) ─────────────────────────────────────────
    // Shares the channel's single running ffmpeg encode via the tee muxer
    // (ChannelSession::hlsDir()) — same session as /stream/channels/:id, just
    // a second output. Track selection is the channel's admin-configured
    // audio_lang/subtitle_lang, not per-viewer (see plan: live is broadcast).
    svr.Get(R"(/stream/hls/channels/([^/]+)/playlist\.m3u8$)", [&sessions](
            const httplib::Request& req, httplib::Response& res) {
        auto session = sessions.getOrCreate(req.matches[1]);
        if (!session) { res.status = 503; res.set_content(json{{"error","channel unavailable"}}.dump(), "application/json"); return; }
        session->touchHls();
        auto path = session->hlsDir() + "/playlist.m3u8";
        if (!waitForFile(path)) { res.status = 503; res.set_content(json{{"error","not ready"}}.dump(), "application/json"); return; }
        serveHlsFile(path, "application/vnd.apple.mpegurl", res);
    });

    svr.Get(R"(/stream/hls/channels/([^/]+)/(seg-[0-9]+\.ts)$)", [&sessions](
            const httplib::Request& req, httplib::Response& res) {
        auto session = sessions.getOrCreate(req.matches[1]);
        if (!session) { res.status = 404; return; }
        session->touchHls();
        serveHlsFile(session->hlsDir() + "/" + req.matches[2].str(), "video/mp2t", res);
    });

    // ── VOD (on-demand library playback) ──────────────────────────────────────
    // One session per viewer. Seek and track-switch are both "stop this
    // session, start a fresh one at the new position/track" — see the plan's
    // "seek is a new session" design note.
    svr.Post("/stream/vod/start", [&kairos, &vodSessions](
            const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400; res.set_content(json{{"error","invalid JSON"}}.dump(), "application/json"); return;
        }
        auto content_type = body.value("content_type", "");
        auto content_id   = body.value("content_id", "");
        if (content_type != "movie" && content_type != "episode") {
            res.status = 400; res.set_content(json{{"error","content_type must be movie or episode"}}.dump(), "application/json"); return;
        }

        auto info = kairos.getPlaybackInfo(content_type, content_id);
        if (!info || info->file_path.empty()) {
            res.status = 404; res.set_content(json{{"error","content not found"}}.dump(), "application/json"); return;
        }

        int audio_track    = body.value("audio_track", -1);
        int subtitle_track = body.value("subtitle_track", -1);
        int64_t position_ms = body.value("position_ms", int64_t(0));
        if (position_ms < 0) position_ms = 0;

        auto session = vodSessions.create(info->file_path, position_ms, audio_track, subtitle_track);
        if (!session) {
            res.status = 500; res.set_content(json{{"error","failed to start playback"}}.dump(), "application/json"); return;
        }

        json tracks;
        tracks["video"] = json::array();
        for (auto& t : session->tracks().video)
            tracks["video"].push_back({{"codec", t.codec}, {"width", t.width}, {"height", t.height}});
        tracks["audio"] = json::array();
        for (auto& t : session->tracks().audio)
            tracks["audio"].push_back({{"index", t.relative_index}, {"codec", t.codec}, {"language", t.language}, {"title", t.title}, {"channels", t.channels}});
        tracks["subtitles"] = json::array();
        for (auto& t : session->tracks().subtitles)
            tracks["subtitles"].push_back({{"index", t.relative_index}, {"codec", t.codec}, {"language", t.language}, {"title", t.title},
                                            {"extractable", t.codec == "subrip" || t.codec == "ass" || t.codec == "ssa" || t.codec == "mov_text" || t.codec == "webvtt" || t.codec == "text"}});

        json out = {
            {"session_id",   session->sessionId()},
            {"manifest_url", "/stream/vod/" + session->sessionId() + "/playlist.m3u8"},
            {"direct_play",  session->directPlay()},
            {"duration_ms",  info->duration_ms},
            {"title",        info->title},
            {"tracks",       tracks},
        };
        if (session->hasSubtitleOutput())
            out["subtitle_url"] = "/stream/vod/" + session->sessionId() + "/subs.vtt";
        res.set_content(out.dump(), "application/json");
    });

    svr.Get(R"(/stream/vod/([^/]+)/playlist\.m3u8$)", [&vodSessions](
            const httplib::Request& req, httplib::Response& res) {
        auto session = vodSessions.get(req.matches[1]);
        if (!session) { res.status = 404; res.set_content(json{{"error","session not found"}}.dump(), "application/json"); return; }
        session->touch();
        auto path = session->dir() + "/playlist.m3u8";
        if (!waitForFile(path)) { res.status = 503; res.set_content(json{{"error","not ready"}}.dump(), "application/json"); return; }
        serveHlsFile(path, "application/vnd.apple.mpegurl", res);
    });

    svr.Get(R"(/stream/vod/([^/]+)/(seg-[0-9]+\.ts)$)", [&vodSessions](
            const httplib::Request& req, httplib::Response& res) {
        auto session = vodSessions.get(req.matches[1]);
        if (!session) { res.status = 404; return; }
        session->touch();
        serveHlsFile(session->dir() + "/" + req.matches[2].str(), "video/mp2t", res);
    });

    svr.Get(R"(/stream/vod/([^/]+)/subs\.vtt$)", [&vodSessions](
            const httplib::Request& req, httplib::Response& res) {
        auto session = vodSessions.get(req.matches[1]);
        if (!session) { res.status = 404; return; }
        session->touch();
        auto path = session->dir() + "/subs.vtt";
        if (!waitForFile(path)) { res.status = 503; return; }
        serveHlsFile(path, "text/vtt", res);
    });

    svr.Post(R"(/stream/vod/([^/]+)/stop$)", [&vodSessions](
            const httplib::Request& req, httplib::Response& res) {
        vodSessions.stop(req.matches[1]);
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    // ── Preview (Guide hover previews) ────────────────────────────────────────
    svr.Post("/stream/preview/start", [&previewSessions](
            const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400; res.set_content(json{{"error","invalid JSON"}}.dump(), "application/json"); return;
        }
        auto channel_id = body.value("channel_id", "");
        if (channel_id.empty()) {
            res.status = 400; res.set_content(json{{"error","channel_id required"}}.dump(), "application/json"); return;
        }
        auto session = previewSessions.create(channel_id);
        if (!session) {
            res.status = 500; res.set_content(json{{"error","failed to start preview"}}.dump(), "application/json"); return;
        }
        res.set_content(json{
            {"session_id",   session->sessionId()},
            {"manifest_url", "/stream/preview/" + session->sessionId() + "/playlist.m3u8"},
        }.dump(), "application/json");
    });

    svr.Post(R"(/stream/preview/([^/]+)/switch$)", [&previewSessions](
            const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400; res.set_content(json{{"error","invalid JSON"}}.dump(), "application/json"); return;
        }
        auto channel_id = body.value("channel_id", "");
        if (channel_id.empty()) {
            res.status = 400; res.set_content(json{{"error","channel_id required"}}.dump(), "application/json"); return;
        }
        if (!previewSessions.switchChannel(req.matches[1], channel_id)) {
            res.status = 404; res.set_content(json{{"error","session not found or switch failed"}}.dump(), "application/json"); return;
        }
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    svr.Get(R"(/stream/preview/([^/]+)/playlist\.m3u8$)", [&previewSessions](
            const httplib::Request& req, httplib::Response& res) {
        auto session = previewSessions.get(req.matches[1]);
        if (!session) { res.status = 404; res.set_content(json{{"error","session not found"}}.dump(), "application/json"); return; }
        session->touch();
        auto path = session->dir() + "/playlist.m3u8";
        if (!waitForFile(path)) { res.status = 503; res.set_content(json{{"error","not ready"}}.dump(), "application/json"); return; }
        serveHlsFile(path, "application/vnd.apple.mpegurl", res);
    });

    svr.Get(R"(/stream/preview/([^/]+)/(seg-[0-9]+\.ts)$)", [&previewSessions](
            const httplib::Request& req, httplib::Response& res) {
        auto session = previewSessions.get(req.matches[1]);
        if (!session) { res.status = 404; return; }
        session->touch();
        serveHlsFile(session->dir() + "/" + req.matches[2].str(), "video/mp2t", res);
    });

    svr.Post(R"(/stream/preview/([^/]+)/stop$)", [&previewSessions](
            const httplib::Request& req, httplib::Response& res) {
        previewSessions.stop(req.matches[1]);
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });
}
