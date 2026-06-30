#include "ChannelBroadcaster.h"
#include <httplib.h>
#include <iostream>
#include <algorithm>

ChannelBroadcaster::ChannelBroadcaster(std::string id, std::string heph_url, int linger_secs)
    : id_(std::move(id)), heph_url_(std::move(heph_url)), linger_secs_(linger_secs) {}

ChannelBroadcaster::~ChannelBroadcaster() {
    stop_requested_.store(true);
    if (reader_.joinable()) reader_.join();
}

std::shared_ptr<ChannelBroadcaster::Sink> ChannelBroadcaster::addClient() {
    std::lock_guard lock(sinks_mtx_);
    ++stop_token_;  // cancel any pending linger

    // Join the old thread if it exited cleanly (dead but not yet joined).
    if (reader_.joinable() && dead_.load()) {
        reader_.join();
    }

    if (!reader_.joinable()) {
        stop_requested_.store(false);
        dead_.store(false);
        reader_ = std::thread([this] { run(); });
    }

    auto sink = std::make_shared<Sink>();
    sinks_.push_back(sink);
    return sink;
}

void ChannelBroadcaster::removeClient(std::shared_ptr<Sink> sink) {
    {
        std::lock_guard lock(sinks_mtx_);
        sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
        if (!sinks_.empty()) return;
    }
    scheduleStop();
}

bool ChannelBroadcaster::hasClients() const {
    std::lock_guard lock(sinks_mtx_);
    return !sinks_.empty();
}

void ChannelBroadcaster::run() {
    std::string path = "/stream/channels/" + id_;

    static constexpr int MAX_ATTEMPTS  = 5;
    static constexpr int RETRY_BASE_MS = 500; // backoff: 500 ms, 1 s, 1.5 s, 2 s

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
        if (stop_requested_.load()) break;

        httplib::Client cli(heph_url_);
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(30, 0);

        if (attempt == 1)
            std::cout << "[hermes] connecting to " << heph_url_ << path << "\n";
        else
            std::cerr << "[hermes] channel " << id_ << " retry " << attempt
                      << " → " << heph_url_ << path << "\n";

        bool status_ok = false;
        auto result = cli.Get(
            path,
            [&status_ok](const httplib::Response& resp) {
                status_ok = (resp.status == 200);
                return status_ok;
            },
            [this](const char* data, size_t len) {
                if (stop_requested_.load()) return false;
                broadcast(data, len);
                return true;
            }
        );

        if (status_ok) break; // stream ran (or was intentionally stopped mid-stream)

        if (!result || result.error() != httplib::Error::Success) {
            std::cerr << "[hermes] channel " << id_ << " stream error: "
                      << httplib::to_string(result.error()) << "\n";
        } else {
            std::cerr << "[hermes] channel " << id_ << " Hephaestus returned non-200\n";
        }

        if (attempt < MAX_ATTEMPTS && !stop_requested_.load()) {
            int delay_ms = RETRY_BASE_MS * attempt;
            std::cerr << "[hermes] channel " << id_ << " retrying in " << delay_ms << " ms\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }

    broadcastDone();
    dead_.store(true);
    std::cout << "[hermes] broadcaster for channel " << id_ << " exited\n";
}

void ChannelBroadcaster::broadcast(const char* data, size_t len) {
    std::vector<uint8_t> chunk(data, data + len);
    std::lock_guard lock(sinks_mtx_);
    for (auto& sink : sinks_) {
        std::lock_guard slock(sink->mtx);
        if (sink->queue.size() >= Sink::MAX_QUEUE) {
            sink->queue.pop_front();
        }
        sink->queue.push_back(chunk);
        sink->cv.notify_one();
    }
}

void ChannelBroadcaster::broadcastDone() {
    std::lock_guard lock(sinks_mtx_);
    for (auto& sink : sinks_) {
        std::lock_guard slock(sink->mtx);
        sink->done.store(true);
        sink->cv.notify_one();
    }
}

void ChannelBroadcaster::scheduleStop() {
    uint32_t token = ++stop_token_;
    int linger = linger_secs_;
    std::thread([this, token, linger] {
        std::this_thread::sleep_for(std::chrono::seconds(linger));
        if (stop_token_.load() != token) return;
        std::lock_guard lock(sinks_mtx_);
        if (!sinks_.empty()) return;
        stop_requested_.store(true);
    }).detach();
}
