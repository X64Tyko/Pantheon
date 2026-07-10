#include "WatchProgressRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>

WatchProgressRepository::WatchProgressRepository(Database& db) : db_(db) {}

void WatchProgressRepository::upsert(const std::string& user_id,
                                     const std::string& content_type,
                                     const std::string& content_id,
                                     int64_t position_ms,
                                     int64_t duration_ms,
                                     int64_t updated_at) {
    SQLite::Statement ins(db_.get(), R"SQL(
        INSERT INTO watch_progress (user_id, content_type, content_id, position_ms, duration_ms, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(user_id, content_type, content_id) DO UPDATE SET
            position_ms = excluded.position_ms,
            duration_ms = excluded.duration_ms,
            updated_at  = excluded.updated_at
    )SQL");
    ins.bind(1, user_id);
    ins.bind(2, content_type);
    ins.bind(3, content_id);
    ins.bind(4, position_ms);
    ins.bind(5, duration_ms);
    ins.bind(6, updated_at);
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
