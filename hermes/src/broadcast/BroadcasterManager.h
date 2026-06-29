#pragma once
#include "ChannelBroadcaster.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>

class BroadcasterManager {
public:
    BroadcasterManager(std::string heph_url, int linger_secs);

    // Returns an existing live broadcaster or creates a new one.
    std::shared_ptr<ChannelBroadcaster> getOrCreate(const std::string& channel_id);

    // Remove broadcasters that have exited and have no clients.
    void reap();

private:
    std::string heph_url_;
    int         linger_secs_;

    std::mutex mtx_;
    std::map<std::string, std::shared_ptr<ChannelBroadcaster>> map_;
};
