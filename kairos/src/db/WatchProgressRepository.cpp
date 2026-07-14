#include "WatchProgressRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>

WatchProgressRepository::WatchProgressRepository(Database& db) : db_(db) {}

// `completed` is the rewatch count itself, not a 0/1 flag — see its column
// comment. The increment condition can't check "was the stored count 0"
// (it's never reset back to 0), so it instead checks whether the row's own
// position/duration, before this write, was already sitting at a finished
// position (bare `position_ms`/`duration_ms` = pre-update values under
// SQLite's UPSERT semantics; `excluded.*` = the incoming write). That's a
// more reliable "did this ping just cross the finish line" signal than a
// flag anyway, since it can't drift out of sync with the actual position.
void WatchProgressRepository::upsert(const std::string& user_id,
                                     const std::string& content_type,
                                     const std::string& content_id,
                                     int64_t position_ms,
                                     int64_t duration_ms,
                                     int64_t updated_at,
                                     bool completed) {
    SQLite::Statement ins(db_.get(), R"SQL(
        INSERT INTO watch_progress (user_id, content_type, content_id, position_ms, duration_ms, updated_at, completed)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(user_id, content_type, content_id) DO UPDATE SET
            position_ms = excluded.position_ms,
            duration_ms = excluded.duration_ms,
            updated_at  = excluded.updated_at,
            completed   = completed + CASE
                WHEN ?8 = 1 AND NOT (duration_ms > 0 AND position_ms >= duration_ms)
                THEN 1 ELSE 0 END
    )SQL");
    ins.bind(1, user_id);
    ins.bind(2, content_type);
    ins.bind(3, content_id);
    ins.bind(4, position_ms);
    ins.bind(5, duration_ms);
    ins.bind(6, updated_at);
    ins.bind(7, completed ? 1 : 0); // insert-branch seed: initial count is 1 iff the very first write is already a finish
    ins.bind(8, completed ? 1 : 0); // same "this ping just finished" bool, reused in the UPDATE branch's CASE
    ins.exec();
}

void WatchProgressRepository::remove(const std::string& user_id,
                                     const std::string& content_type,
                                     const std::string& content_id) {
    SQLite::Statement del(db_.get(),
        "DELETE FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");
    del.bind(1, user_id);
    del.bind(2, content_type);
    del.bind(3, content_id);
    del.exec();
}

int64_t WatchProgressRepository::getUpdatedAt(const std::string& user_id,
                                              const std::string& content_type,
                                              const std::string& content_id) {
    SQLite::Statement q(db_.get(),
        "SELECT updated_at FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");
    q.bind(1, user_id);
    q.bind(2, content_type);
    q.bind(3, content_id);
    if (q.executeStep()) return q.getColumn(0).getInt64();
    return 0;
}

std::optional<ShowWatchStateRow> WatchProgressRepository::getLatestForShow(
        const std::string& user_id, const std::string& show_id) {
    // wp.completed is the rewatch count now, not a 0/1 "currently finished"
    // flag — it never resets when a rewatch starts, so it can't answer this
    // row's actual question ("is the most-recently-touched episode fully
    // finished right now, or still in progress"). That's computed fresh from
    // position vs duration instead; ShowWatchStateRow::completed keeps its
    // original current-state meaning for every caller downstream.
    SQLite::Statement q(db_.get(), R"SQL(
        SELECT wp.content_id, wp.position_ms, wp.duration_ms,
               (wp.duration_ms > 0 AND wp.position_ms >= wp.duration_ms) AS completed,
               wp.updated_at
        FROM watch_progress wp
        JOIN episode e ON e.episode_id = wp.content_id
        WHERE wp.user_id = ? AND wp.content_type = 'episode' AND e.show_id = ?
        ORDER BY wp.updated_at DESC LIMIT 1
    )SQL");
    q.bind(1, user_id);
    q.bind(2, show_id);
    if (!q.executeStep()) return std::nullopt;

    ShowWatchStateRow r;
    r.content_id  = q.getColumn(0).getString();
    r.position_ms = q.getColumn(1).getInt64();
    r.duration_ms = q.getColumn(2).getInt64();
    r.completed   = q.getColumn(3).getInt() != 0;
    r.updated_at  = q.getColumn(4).getInt64();
    return r;
}
