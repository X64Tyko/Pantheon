#pragma once
#include <cstdint>
#include <string>

class Database;

class WatchProgressRepository {
public:
    explicit WatchProgressRepository(Database& db);

    // Upserts position. Callers deciding whether an incoming write should
    // win over an existing row (e.g. SyncManager's freshness-gated seed from
    // a source) must check that themselves — this always writes.
    void upsert(const std::string& user_id,
               const std::string& content_type,
               const std::string& content_id,
               int64_t position_ms,
               int64_t duration_ms,
               int64_t updated_at);

    void remove(const std::string& user_id,
               const std::string& content_type,
               const std::string& content_id);

    // updated_at of the existing row, or 0 if there is none. Used by callers
    // (SyncManager) to decide whether a source's watch state is fresher than
    // what's already recorded before overwriting it.
    int64_t getUpdatedAt(const std::string& user_id,
                         const std::string& content_type,
                         const std::string& content_id);

private:
    Database& db_;
};
