#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

// One item's watch state for PlaylistRepository::getStates' batch lookup —
// `completed` is the same fresh position-vs-duration derivation as
// ShowWatchStateRow.completed, not the stored rewatch-count column.
struct WatchStateRow {
    int64_t position_ms = 0;
    int64_t duration_ms = 0;
    bool    completed   = false;
};

// Shared key builder so getStates' caller can look up the same map it
// populates without duplicating the "\x1f"-join convention.
inline std::string watchStateKey(const std::string& content_type, const std::string& content_id) {
    return content_type + "\x1f" + content_id;
}

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

    // Batch lookup for playlist resume (PlaylistService's resolve-play-target)
    // — one query instead of one round-trip per item. `items` is
    // {content_type, content_id} pairs; keyed in the result by
    // "content_type\x1f content_id" since a movie and an episode could in
    // principle share the same raw id string across their separate tables.
    // Items with no watch_progress row at all are simply absent from the map.
    std::unordered_map<std::string, WatchStateRow> getStates(
        const std::string& user_id,
        const std::vector<std::pair<std::string, std::string>>& items);

private:
    Database& db_;
};
