#pragma once
#include "WatchTogetherSession.h"
#include "../Config.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>

// Live Watch Together sessions, keyed by the Kairos-issued session_id.
// Mirrors DeviceSessionManager/BroadcasterManager (map<id, shared_ptr>,
// mutex-guarded, getOrCreate/reap) — Kairos, not Hermes, is the source of
// truth for whether a session_id is real and who hosts it, so getOrCreate on
// a cache miss asks Kairos rather than trusting whatever the caller claims.
class WatchTogetherManager {
public:
    // Returns the live session for session_id, creating it on first touch —
    // a lookup against Kairos's GET /api/watch-together/:id for host_user_id
    // (using caller_auth_header, whatever bearer token this particular
    // request happened to carry, purely to satisfy Kairos's own "must be
    // logged in" gate on that route — not necessarily the host's own token).
    // nullptr if Kairos doesn't know this session_id, or it's already closed
    // there — either way there's nothing to coordinate.
    std::shared_ptr<WatchTogetherSession> getOrCreate(const std::string& session_id,
                                                        const Config& cfg,
                                                        const std::string& caller_auth_header);

    // Drops (and best-effort closes on Kairos, via whichever host bearer
    // token this session last saw — see WatchTogetherSession::hostAuthHeader)
    // any session whose host hasn't sent a command/heartbeat in host_grace_ms.
    void reap(int64_t host_grace_ms, const Config& cfg);

    // Appends a `sync` event to every live session — see
    // WatchTogetherSession::appendSyncEvent's own comment on why this runs
    // independent of host command traffic.
    void tickSync();

private:
    std::mutex mtx_;
    std::map<std::string, std::shared_ptr<WatchTogetherSession>> map_;
};
