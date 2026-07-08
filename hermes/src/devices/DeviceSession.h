#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

// One live connection from a running Roku channel, keyed by its persistent
// roku_device_id (see DeviceSessionManager) — created once the channel has
// confirmed pairing (or, on later boots, already knows its own id from
// roRegistry). Holds a queue of pending commands (caster -> Roku) and the
// latest reported playback state (Roku -> caster); mirrors
// ChannelBroadcaster's Sink pattern (mutex + condvar + deque) but for JSON
// messages instead of raw media bytes.
class DeviceSession {
public:
    DeviceSession(std::string roku_device_id, std::string user_id);

    const std::string& rokuDeviceId() const { return roku_device_id_; }
    const std::string& userId()      const { return user_id_; }

    // Caster side: enqueue a command, wake any blocked long-poll.
    void pushCommand(nlohmann::json command);

    // Roku side: blocks up to timeout_ms for a command; nullopt on timeout.
    // Counts as a heartbeat (updates lastSeenMs) regardless of outcome.
    std::optional<nlohmann::json> waitForCommand(int timeout_ms);

    // Roku side: replace latest reported state. Also a heartbeat.
    void setState(nlohmann::json state);
    // Caster side: latest state (empty object if the Roku hasn't reported yet).
    nlohmann::json state() const;

    void    touch();
    int64_t lastSeenMs() const { return last_seen_ms_.load(); }

    static int64_t nowMs();

private:
    std::string roku_device_id_;
    std::string user_id_;

    std::mutex                 mtx_;
    std::condition_variable    cv_;
    std::deque<nlohmann::json> commands_;

    mutable std::mutex state_mtx_;
    nlohmann::json     state_ = nlohmann::json::object();

    std::atomic<int64_t> last_seen_ms_;
};
