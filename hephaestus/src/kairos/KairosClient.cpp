#include "KairosClient.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

KairosClient::KairosClient(std::string base_url) : base_url(std::move(base_url)) {}

static httplib::Client makeClient(const std::string& url) {
    httplib::Client c(url);
    c.set_connection_timeout(5);
    c.set_read_timeout(10);
    return c;
}

std::optional<KairosNowResponse> KairosClient::getNow(const std::string& channelId, int64_t atMs) {
    auto cli = makeClient(base_url);
    std::string path = "/api/channels/" + channelId + "/now";
    if (atMs >= 0) path += "?at=" + std::to_string(atMs);

    auto res = cli.Get(path);
    if (!res || res->status != 200) {
        std::cerr << "[kairos] GET " << path << " -> "
                  << (res ? std::to_string(res->status) : "no response") << "\n";
        return std::nullopt;
    }

    try {
        auto j = json::parse(res->body);
        KairosNowResponse r;
        r.item_type            = j.value("item_type",           "");
        r.item_id              = j.value("item_id",             "");
        r.file_path            = j.value("file_path",           "");
        r.title                = j.value("title",               "");
        r.block_id             = j.value("block_id",            "");
        r.duration_ms          = j.value("duration_ms",         int64_t(0));
        r.wall_clock_start_ms  = j.value("wall_clock_start_ms", int64_t(0));
        r.wall_clock_end_ms    = j.value("wall_clock_end_ms",   int64_t(0));
        r.is_filler            = j.value("is_filler",           false);

        auto optStr = [&](const char* k) -> std::optional<std::string> {
            if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
            return std::nullopt;
        };
        r.show_title  = optStr("show_title");
        r.show_id     = optStr("show_id");
        r.source_id   = optStr("source_id");
        r.external_id = optStr("external_id");

        if (j.contains("season")      && j["season"].is_number())      r.season      = j["season"].get<int>();
        if (j.contains("episode_num") && j["episode_num"].is_number()) r.episode_num = j["episode_num"].get<int>();
        return r;
    } catch (const std::exception& e) {
        std::cerr << "[kairos] getNow JSON parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

void KairosClient::markPlayed(const std::string& channelId,
                               const std::string& itemType,
                               const std::string& itemId,
                               const std::string& blockId,
                               int64_t durationActualMs) {
    auto cli = makeClient(base_url);
    json body = {
        {"item_type",          itemType},
        {"item_id",            itemId},
        {"block_id",           blockId},
        {"duration_actual_ms", durationActualMs}
    };
    auto res = cli.Post("/api/channels/" + channelId + "/played",
                        body.dump(), "application/json");
    if (!res || res->status / 100 != 2) {
        std::cerr << "[kairos] POST /played failed for channel " << channelId
                  << ": " << (res ? std::to_string(res->status) : "no response") << "\n";
    }
}

std::vector<KairosChannel> KairosClient::getChannels() {
    auto cli = makeClient(base_url);
    auto res = cli.Get("/api/channels");
    if (!res || res->status != 200) {
        std::cerr << "[kairos] GET /api/channels -> "
                  << (res ? std::to_string(res->status) : "no response") << "\n";
        return {};
    }
    try {
        auto j = json::parse(res->body);
        std::vector<KairosChannel> channels;
        for (auto& item : j) {
            KairosChannel ch;
            ch.channel_id = item.value("channel_id", "");
            ch.name       = item.value("name",       "");
            ch.number     = item.value("number",     0);
            if (!ch.channel_id.empty()) channels.push_back(std::move(ch));
        }
        return channels;
    } catch (const std::exception& e) {
        std::cerr << "[kairos] getChannels JSON parse error: " << e.what() << "\n";
        return {};
    }
}
