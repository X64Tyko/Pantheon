#pragma once
#include "DeviceSession.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Live Roku device sessions, keyed by persistent roku_device_id. Mirrors
// BroadcasterManager (map<id, shared_ptr<Session>>, mutex-guarded,
// getOrCreate/reap) — the streaming-fan-out equivalent for command/state
// messages instead of media bytes.
class DeviceSessionManager {
public:
    // Creates the live session if none exists yet, else refreshes (touches)
    // the existing one — a device reconnecting after a restart gets a fresh
    // session, same as BroadcasterManager::getOrCreate against a dead entry.
    std::shared_ptr<DeviceSession> getOrCreate(const std::string& roku_device_id, const std::string& user_id);

    // Null if this device isn't currently connected.
    std::shared_ptr<DeviceSession> get(const std::string& roku_device_id);

    std::vector<std::shared_ptr<DeviceSession>> listForUser(const std::string& user_id);

    // Every live session regardless of owner — admin-only "who's connected"
    // view (DeviceRouter.cpp's GET /api/devices/all), unlike listForUser
    // above (the cast-device-picker's "my own devices" scope).
    std::vector<std::shared_ptr<DeviceSession>> listAll();

    // Drops sessions whose long-poll/state-post hasn't reconnected within grace_ms.
    void reap(int64_t grace_ms);

private:
    std::mutex mtx_;
    std::map<std::string, std::shared_ptr<DeviceSession>> map_;
};
