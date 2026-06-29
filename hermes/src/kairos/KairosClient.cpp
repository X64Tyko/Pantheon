#include "KairosClient.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

KairosClient::KairosClient(std::string base_url) : base_url_(std::move(base_url)) {}

std::vector<KairosChannel> KairosClient::getChannels() {
    httplib::Client cli(base_url_);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

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
        std::cerr << "[kairos] getChannels parse error: " << e.what() << "\n";
        return {};
    }
}
