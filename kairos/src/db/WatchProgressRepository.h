#pragma once
#include <cstdint>
#include <optional>
#include <string>

class Database;

// Most-recently-touched watch_progress row for one show's episodes,
// regardless of completed state — lets a caller (resolvePlayTarget) tell
// "resume mid-episode" apart from "last episode was finished, continue at
// the next one" instead of only ever seeing still-in-progress rows.
struct ShowWatchStateRow {
    std::string content_id;
    int64_t     position_ms = 0;
    int64_t     duration_ms = 0;
    bool        completed   = false;
    int64_t     updated_at  = 0;
};

class WatchProgressRepository {
public:
    explicit WatchProgressRepository(Database& db);

    // Upserts position/duration/updated_at. `completed` is this specific
    // write's "did this ping just cross the finish line" signal — the stored
    // watch_progress.completed column is the rewatch count itself (not a 0/1
    // flag), incremented by exactly 1 when `completed=true` and the row
    // wasn't already sitting at a finished position. Callers deciding whether
    // an incoming write should win over an existing row (e.g. SyncManager's
    // freshness-gated seed from a source) must check that themselves — this
    // always writes.
    void upsert(const std::string& user_id,
               const std::string& content_type,
               const std::string& content_id,
               int64_t position_ms,
               int64_t duration_ms,
               int64_t updated_at,
               bool completed);

    void remove(const std::string& user_id,
               const std::string& content_type,
               const std::string& content_id);

    // updated_at of the existing row, or 0 if there is none. Used by callers
    // (SyncManager) to decide whether a source's watch state is fresher than
    // what's already recorded before overwriting it.
    int64_t getUpdatedAt(const std::string& user_id,
                         const std::string& content_type,
                         const std::string& content_id);

    // Most recent (by updated_at) episode watch_progress row for any episode
    // belonging to show_id, completed or not.
    std::optional<ShowWatchStateRow> getLatestForShow(const std::string& user_id,
                                                       const std::string& show_id);

private:
    Database& db_;
};
