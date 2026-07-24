#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

// One live Watch Together group's playback coordination state — created
// lazily (see WatchTogetherManager::getOrCreate) the first time anyone
// touches a Kairos-issued session_id, torn down once the host has been gone
// past the reap grace period. Kairos owns identity/discovery (who's hosting,
// what content, is it still open); this class owns exactly the live state
// Kairos deliberately never persists — position/paused and who's watched it
// most recently — mirroring DeviceSession's shape (mutex-guarded state +
// something to wait on) but broadcasting to N followers via the same
// seq+waitAfter pull model LogBuffer/`/api/logs/stream` already use (see
// shared/log/LogBuffer.h) instead of a single Roku's own private long-poll
// queue: every subscriber tracks its own `after_seq` cursor against one
// shared event log rather than each getting a private queue.
//
// Correction is active, not just reactive to host actions — see the design
// thread this came out of. The host's client posts a lightweight periodic
// heartbeat (position_ms, paused); WatchTogetherManager's timer thread turns
// that into a recurring `sync` event on this session's own log independent
// of whether the host has issued any explicit seek/pause/play recently, so a
// follower who stalled on a rebuffer or just joined gets pulled to the real
// position without the host needing to touch anything.
class WatchTogetherSession {
public:
    WatchTogetherSession(std::string session_id, std::string host_user_id);

    const std::string& sessionId()  const { return session_id_; }
    const std::string& hostUserId() const { return host_user_id_; }

    // Host-only explicit action — {"type": "seek"|"pause"|"play", "position_ms"?}.
    // Becomes the new authoritative position/paused baseline immediately
    // (not just eventually via the next sync tick) AND is appended to the
    // event log verbatim so every follower applies it right away.
    void pushCommand(const nlohmann::json& command);

    // Host-only, frequent (a few seconds), cheap — updates the baseline used
    // to extrapolate authoritativePositionMs() between explicit commands and
    // refreshes host liveness (see hostIdleMs()). Does NOT append an event
    // of its own; WatchTogetherManager's periodic tick is what actually
    // broadcasts the resulting position out to followers, on its own
    // schedule, so N heartbeats a minute don't become N broadcast events.
    void heartbeat(int64_t position_ms, bool paused);

    // Appends a `sync` event carrying the current extrapolated authoritative
    // position/paused state — called by WatchTogetherManager's timer thread.
    void appendSyncEvent();

    // A fresh subscriber's first message (see WatchTogetherManager's stream
    // route) — one synthesized `sync` event for the CURRENT state, not a
    // backlog of past commands (a seek from ten minutes ago means nothing to
    // someone joining now). Second element is the seq to start waitAfter()
    // from.
    std::pair<nlohmann::json, uint64_t> initialSnapshot() const;

    // Same contract as LogBuffer::waitAfter() (shared/log/LogBuffer.h) — one
    // or more events newer than after_seq, or empty on timeout.
    std::pair<std::vector<nlohmann::json>, uint64_t>
    waitAfter(uint64_t after_seq, std::chrono::milliseconds timeout) const;

    // Extrapolated live position right now: the last confirmed position_ms
    // plus elapsed wall-clock time since it was confirmed while playing,
    // flat while paused.
    int64_t authoritativePositionMs() const;
    bool    isPaused() const;

    // Wall-clock ms since the host's last command/heartbeat — reap()'s own
    // "host gone" signal (WatchTogetherManager).
    int64_t hostIdleMs() const;

    // The bearer token last seen proving itself as this session's host (via
    // pushCommand/heartbeat) — Hermes has no service-account token of its
    // own to call Kairos's admin/host-gated POST /api/watch-together/:id/close
    // with when reaping, so it reuses whichever real host token it most
    // recently saw instead. Empty until the host has made at least one
    // command/heartbeat call.
    std::string hostAuthHeader() const;
    void setHostAuthHeader(std::string header);

    static int64_t nowMs();

private:
    std::string session_id_;
    std::string host_user_id_;

    mutable std::mutex host_auth_mtx_;
    std::string        host_auth_header_;

    // Guards position_ms_/paused_/updated_at_ms_ together — extrapolation
    // needs a consistent snapshot of all three, not three independent
    // atomics that could be read mid-update.
    mutable std::mutex state_mtx_;
    int64_t position_ms_   = 0;
    bool    paused_        = true;
    int64_t updated_at_ms_ = 0;

    std::atomic<int64_t> host_last_seen_ms_{0};

    struct Entry { uint64_t seq; nlohmann::json event; };
    // Small — followers only ever need "what's current", not history; a
    // handful of entries covers a slow poller missing one tick, nothing more
    // is meaningful once a genuinely new sync tick has superseded it.
    static constexpr size_t kMaxBacklog = 20;

    mutable std::mutex              log_mtx_;
    mutable std::condition_variable log_cv_;
    std::deque<Entry>               entries_;
    uint64_t                        seq_ = 1; // 0 is the "nothing seen yet" sentinel

    void appendEvent(nlohmann::json event);
};
