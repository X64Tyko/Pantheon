#include "Router.h"
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

void registerRoutes(httplib::Server& svr, SessionManager& sessions,
                    KairosClient& kairos, LogBuffer& logs) {

    // ── Health ────────────────────────────────────────────────────────────────
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"status", "ok", "service", "hephaestus"}}.dump(), "application/json");
    });

    // ── Log stream (SSE, same contract as Kairos /api/logs/stream) ────────────
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

    // ── Channel list (proxied from Kairos) ────────────────────────────────────
    svr.Get("/api/channels", [&kairos](const httplib::Request&, httplib::Response& res) {
        auto channels = kairos.getChannels();
        json arr = json::array();
        for (auto& ch : channels) {
            arr.push_back({{"channel_id", ch.channel_id},
                           {"name",       ch.name},
                           {"number",     ch.number}});
        }
        res.set_content(arr.dump(), "application/json");
    });

    // ── MPEG-TS stream ────────────────────────────────────────────────────────
    // GET /stream/channels/:channelId
    // The channelId here is the Kairos channel UUID.
    //
    // Each request attaches to the shared ChannelSession for that channel.
    // The session persists across client connect/disconnect (linger period),
    // so clients always join a live stream mid-segment rather than restarting
    // from the beginning of the current item.
    svr.Get(R"(/stream/channels/([^/]+))", [&sessions](
            const httplib::Request& req, httplib::Response& res) {

        std::string channel_id = req.matches[1];
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
            [sink](size_t /*offset*/, httplib::DataSink& data_sink) -> bool {
                std::unique_lock<std::mutex> lock(sink->mtx);
                sink->cv.wait(lock, [&] {
                    return !sink->queue.empty() || sink->done.load();
                });

                if (sink->queue.empty()) {
                    // Session ended
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
            [sink, session](bool /*success*/) {
                session->removeClient(sink);
            }
        );
    });

    // ── M3U playlist (for VLC / IPTV players) ─────────────────────────────────
    svr.Get("/playlist.m3u", [&kairos](const httplib::Request& req, httplib::Response& res) {
        auto host = req.get_header_value("Host");
        if (host.empty()) host = "localhost:8082";
        auto base = "http://" + host;

        auto channels = kairos.getChannels();
        std::string m3u = "#EXTM3U\n";
        for (auto& ch : channels) {
            m3u += "#EXTINF:-1 tvg-id=\"" + ch.channel_id
                + "\" tvg-name=\"" + ch.name
                + "\" channel-number=\"" + std::to_string(ch.number) + "\","
                + ch.name + "\n"
                + base + "/stream/channels/" + ch.channel_id + "\n";
        }
        res.set_content(m3u, "application/x-mpegurl");
    });
}
