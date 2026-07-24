#include "WatchTogetherSession.h"

int64_t WatchTogetherSession::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

WatchTogetherSession::WatchTogetherSession(std::string session_id, std::string host_user_id)
    : session_id_(std::move(session_id)), host_user_id_(std::move(host_user_id)) {
    updated_at_ms_ = nowMs();
    host_last_seen_ms_.store(nowMs());
}

void WatchTogetherSession::pushCommand(const nlohmann::json& command) {
    auto type = command.value("type", "");
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if (command.contains("position_ms") && command["position_ms"].is_number_integer())
            position_ms_ = command["position_ms"].get<int64_t>();
        if (type == "pause") paused_ = true;
        else if (type == "play") paused_ = false;
        // "seek" leaves paused_ as whatever it already was.
        updated_at_ms_ = nowMs();
    }
    host_last_seen_ms_.store(nowMs());
    appendEvent(command);
}

void WatchTogetherSession::heartbeat(int64_t position_ms, bool paused) {
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        position_ms_   = position_ms;
        paused_        = paused;
        updated_at_ms_ = nowMs();
    }
    host_last_seen_ms_.store(nowMs());
}

void WatchTogetherSession::appendSyncEvent() {
    appendEvent({
        {"type",        "sync"},
        {"position_ms", authoritativePositionMs()},
        {"paused",      isPaused()},
    });
}

int64_t WatchTogetherSession::authoritativePositionMs() const {
    std::lock_guard<std::mutex> lock(state_mtx_);
    if (paused_) return position_ms_;
    return position_ms_ + (nowMs() - updated_at_ms_);
}

bool WatchTogetherSession::isPaused() const {
    std::lock_guard<std::mutex> lock(state_mtx_);
    return paused_;
}

int64_t WatchTogetherSession::hostIdleMs() const {
    return nowMs() - host_last_seen_ms_.load();
}

std::string WatchTogetherSession::hostAuthHeader() const {
    std::lock_guard<std::mutex> lock(host_auth_mtx_);
    return host_auth_header_;
}

void WatchTogetherSession::setHostAuthHeader(std::string header) {
    std::lock_guard<std::mutex> lock(host_auth_mtx_);
    host_auth_header_ = std::move(header);
}

std::pair<nlohmann::json, uint64_t> WatchTogetherSession::initialSnapshot() const {
    nlohmann::json snapshot = {
        {"type",        "sync"},
        {"position_ms", authoritativePositionMs()},
        {"paused",      isPaused()},
    };
    std::lock_guard<std::mutex> lock(log_mtx_);
    // seq_ (not seq_ - 1 / entries_.back().seq) — a subscriber's cursor
    // starts at "everything appended so far", including an event that may
    // have landed between authoritativePositionMs() above and this lock;
    // that's fine, the synthesized snapshot already reflects the latest
    // state either way, and starting the cursor any earlier would just
    // replay it as a second, redundant sync moments later.
    return {snapshot, seq_ - 1};
}

std::pair<std::vector<nlohmann::json>, uint64_t>
WatchTogetherSession::waitAfter(uint64_t after_seq, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(log_mtx_);
    log_cv_.wait_for(lock, timeout, [&] {
        return !entries_.empty() && entries_.back().seq > after_seq;
    });
    if (entries_.empty() || entries_.back().seq <= after_seq) return {{}, after_seq};
    std::vector<nlohmann::json> out;
    for (const auto& e : entries_)
        if (e.seq > after_seq) out.push_back(e.event);
    return {out, entries_.back().seq};
}

void WatchTogetherSession::appendEvent(nlohmann::json event) {
    {
        std::lock_guard<std::mutex> lock(log_mtx_);
        entries_.push_back({seq_++, std::move(event)});
        if (entries_.size() > kMaxBacklog) entries_.pop_front();
    }
    log_cv_.notify_all();
}
