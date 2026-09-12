#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One connection to Hephaestus per channel; fans out raw MPEG-TS bytes to N Hermes clients.
// shared_ptr-owned by BroadcasterManager, whose map entry can be overwritten
// with a fresh instance once this one is dead (see BroadcasterManager.cpp) —
// enable_shared_from_this lets scheduleStop()'s linger thread keep itself
// alive for as long as it runs instead of touching a freed `this`.
class ChannelBroadcaster : public std::enable_shared_from_this<ChannelBroadcaster> {
public:
    struct Sink {
        std::mutex              mtx;
        std::condition_variable cv;
        std::deque<std::vector<uint8_t>> queue;
        std::atomic<bool>       done{false};
        static constexpr size_t MAX_QUEUE = 64;
    };

    ChannelBroadcaster(std::string channel_id, std::string heph_url, int linger_secs);
    ~ChannelBroadcaster();

    // Add a new viewer; starts the Hephaestus connection if not already running.
    std::shared_ptr<Sink> addClient();

    // Remove viewer; schedules stop if this was the last one.
    void removeClient(std::shared_ptr<Sink> sink);

    bool isDead()       const { return dead_.load(); }
    bool hasClients()   const;

private:
    void run();
    void broadcast(const char* data, size_t len);
    void broadcastDone();
    void scheduleStop();

    std::string id_;
    std::string heph_url_;
    int         linger_secs_;

    mutable std::mutex            sinks_mtx_;
    std::vector<std::shared_ptr<Sink>> sinks_;

    std::thread         reader_;
    std::atomic<bool>   stop_requested_{false};
    std::atomic<bool>   dead_{false};
    std::atomic<uint32_t> stop_token_{0};
};
