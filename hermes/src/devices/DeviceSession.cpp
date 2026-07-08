#include "DeviceSession.h"
#include <chrono>

int64_t DeviceSession::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

DeviceSession::DeviceSession(std::string roku_device_id, std::string user_id)
    : roku_device_id_(std::move(roku_device_id)), user_id_(std::move(user_id)),
      last_seen_ms_(nowMs()) {}

void DeviceSession::pushCommand(nlohmann::json command) {
    { std::lock_guard lock(mtx_); commands_.push_back(std::move(command)); }
    cv_.notify_one();
}

std::optional<nlohmann::json> DeviceSession::waitForCommand(int timeout_ms) {
    touch();
    std::unique_lock lock(mtx_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !commands_.empty(); }))
        return std::nullopt;
    nlohmann::json cmd = std::move(commands_.front());
    commands_.pop_front();
    return cmd;
}

void DeviceSession::setState(nlohmann::json state) {
    touch();
    std::lock_guard lock(state_mtx_);
    state_ = std::move(state);
}

nlohmann::json DeviceSession::state() const {
    std::lock_guard lock(state_mtx_);
    return state_;
}

void DeviceSession::touch() {
    last_seen_ms_.store(nowMs());
}
