#include "WatchTogetherManager.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

std::shared_ptr<WatchTogetherSession> WatchTogetherManager::getOrCreate(
        const std::string& session_id, const Config& cfg, const std::string& caller_auth_header) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = map_.find(session_id);
        if (it != map_.end()) return it->second;
    }

    httplib::Client cli(cfg.kairos_url);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);
    auto r = cli.Get(("/api/watch-together/" + session_id).c_str(),
                      httplib::Headers{{"Authorization", caller_auth_header}});
    if (!r || r->status != 200) return nullptr;

    std::string host_user_id;
    try {
        auto j = json::parse(r->body);
        if (!j["closed_at"].is_null()) return nullptr; // already ended — nothing to coordinate
        host_user_id = j.value("host_user_id", "");
    } catch (...) { return nullptr; }
    if (host_user_id.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(mtx_);
    // Another request may have raced this same cache miss and won already.
    auto it = map_.find(session_id);
    if (it != map_.end()) return it->second;
    auto session = std::make_shared<WatchTogetherSession>(session_id, host_user_id);
    map_[session_id] = session;
    return session;
}

void WatchTogetherManager::reap(int64_t host_grace_ms, const Config& cfg) {
    std::vector<std::shared_ptr<WatchTogetherSession>> gone;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto it = map_.begin(); it != map_.end(); ) {
            if (it->second->hostIdleMs() > host_grace_ms) {
                gone.push_back(it->second);
                it = map_.erase(it);
            } else ++it;
        }
    }
    // Best-effort — if this session never saw a real host token (a follower
    // subscribed before the host's first heartbeat, then the host vanished
    // without ever sending one), there's nothing to authenticate the close
    // call with; Kairos's own age-based sweep (WatchTogetherService.cpp)
    // is the backstop for that edge case. Outside the lock — a slow/
    // unreachable Kairos must never hold up the next reap cycle's map_ access.
    for (auto& session : gone) {
        auto auth = session->hostAuthHeader();
        if (auth.empty()) continue;
        try {
            httplib::Client cli(cfg.kairos_url);
            cli.set_connection_timeout(3);
            cli.set_read_timeout(3);
            cli.Post(("/api/watch-together/" + session->sessionId() + "/close").c_str(),
                     httplib::Headers{{"Authorization", auth}}, "", "application/json");
        } catch (...) {
            std::cerr << "[watch-together] failed to close " << session->sessionId() << " on Kairos after reap\n";
        }
    }
}

void WatchTogetherManager::tickSync() {
    std::vector<std::shared_ptr<WatchTogetherSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [id, s] : map_) sessions.push_back(s);
    }
    for (auto& s : sessions) s->appendSyncEvent();
}
