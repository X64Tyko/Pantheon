#include "DeviceSessionManager.h"

std::shared_ptr<DeviceSession>
DeviceSessionManager::getOrCreate(const std::string& roku_device_id, const std::string& user_id) {
    std::lock_guard lock(mtx_);
    auto it = map_.find(roku_device_id);
    if (it != map_.end()) {
        it->second->touch();
        return it->second;
    }
    auto session = std::make_shared<DeviceSession>(roku_device_id, user_id);
    map_[roku_device_id] = session;
    return session;
}

std::shared_ptr<DeviceSession> DeviceSessionManager::get(const std::string& roku_device_id) {
    std::lock_guard lock(mtx_);
    auto it = map_.find(roku_device_id);
    return it != map_.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<DeviceSession>> DeviceSessionManager::listForUser(const std::string& user_id) {
    std::lock_guard lock(mtx_);
    std::vector<std::shared_ptr<DeviceSession>> out;
    for (auto& [id, session] : map_)
        if (session->userId() == user_id) out.push_back(session);
    return out;
}

std::vector<std::shared_ptr<DeviceSession>> DeviceSessionManager::listAll() {
    std::lock_guard lock(mtx_);
    std::vector<std::shared_ptr<DeviceSession>> out;
    out.reserve(map_.size());
    for (auto& [id, session] : map_) out.push_back(session);
    return out;
}

void DeviceSessionManager::reap(int64_t grace_ms) {
    std::lock_guard lock(mtx_);
    const int64_t cutoff = DeviceSession::nowMs() - grace_ms;
    for (auto it = map_.begin(); it != map_.end(); ) {
        if (it->second->lastSeenMs() < cutoff) it = map_.erase(it);
        else ++it;
    }
}
