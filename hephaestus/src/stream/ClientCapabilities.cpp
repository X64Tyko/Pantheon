#include "ClientCapabilities.h"
#include <chrono>

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void ClientCapabilityCache::declare(const std::string& token, ClientCapabilities caps) {
    std::lock_guard<std::mutex> lock(mtx);
    entries[token] = Entry{std::move(caps), nowMs()};
    pruneStaleLocked(kClientCapabilityMaxIdleMs);
}

void ClientCapabilityCache::forget(const std::string& token) {
    std::lock_guard<std::mutex> lock(mtx);
    entries.erase(token);
}

std::optional<ClientCapabilities> ClientCapabilityCache::get(const std::string& token) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = entries.find(token);
    if (it == entries.end()) return std::nullopt;
    it->second.last_seen_ms = nowMs();
    return it->second.caps;
}

void ClientCapabilityCache::pruneStale(int64_t maxIdleMs) {
    std::lock_guard<std::mutex> lock(mtx);
    pruneStaleLocked(maxIdleMs);
}

void ClientCapabilityCache::pruneStaleLocked(int64_t maxIdleMs) {
    int64_t now = nowMs();
    for (auto it = entries.begin(); it != entries.end(); ) {
        if (now - it->second.last_seen_ms > maxIdleMs) it = entries.erase(it);
        else ++it;
    }
}
