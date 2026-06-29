#include "BroadcasterManager.h"

BroadcasterManager::BroadcasterManager(std::string heph_url, int linger_secs)
    : heph_url_(std::move(heph_url)), linger_secs_(linger_secs) {}

std::shared_ptr<ChannelBroadcaster>
BroadcasterManager::getOrCreate(const std::string& channel_id) {
    std::lock_guard lock(mtx_);

    auto it = map_.find(channel_id);
    if (it != map_.end() && !it->second->isDead()) {
        return it->second;
    }

    // Dead or missing — create fresh.
    auto bc = std::make_shared<ChannelBroadcaster>(channel_id, heph_url_, linger_secs_);
    map_[channel_id] = bc;
    return bc;
}

void BroadcasterManager::reap() {
    std::lock_guard lock(mtx_);
    for (auto it = map_.begin(); it != map_.end(); ) {
        if (it->second->isDead() && !it->second->hasClients())
            it = map_.erase(it);
        else
            ++it;
    }
}
