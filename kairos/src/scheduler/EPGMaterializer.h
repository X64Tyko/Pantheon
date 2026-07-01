#pragma once
#include <ctime>
#include <map>
#include <string>
#include <vector>
#include "CursorState.h"
#include "RuleEngine.h"

class Database;

// Divergence: an item in the new projection that differs from what is currently
// committed in scheduled_program at the same wall-clock start time.
struct Divergence {
    std::time_t wall_clock_start;
    std::time_t wall_clock_end;
    std::string block_id;
    std::string prev_item_type;
    std::string prev_item_id;
    std::string new_item_type;
    std::string new_item_id;
};

// Result of a generate() call. Passed to commit() to write to the database.
struct GenerateResult {
    std::vector<ScheduledItem>           items;
    std::vector<Divergence>              divergences;
    CursorState                          cursor_state;
    std::map<std::time_t, std::string>   anchors;
    std::vector<PlayRecord>              play_records;
    std::vector<PlayRecord>              filler_records;
};

class EPGMaterializer {
public:
    EPGMaterializer(Database& db, RuleEngine& engine);

    // Pure in-memory projection from the Monday midnight anchor of `from`.
    // No DB writes. Divergences are populated by comparing new items against
    // what is currently in scheduled_program for the same channel/time range.
    GenerateResult generate(const std::string& channel_id,
                            std::time_t from, int horizon_hours,
                            int seed = -1);

    // Commit a GenerateResult to scheduled_program.
    // INSERT OR IGNOREs items, applies cursor state, persists anchors.
    void commit(const std::string& channel_id,
                std::time_t horizon,
                GenerateResult& result);

    // Ensure the schedule for `channel_id` is populated from `from` through
    // `from + horizon_hours`. Calls generate() + commit() for scheduled mode;
    // handles on_play mode separately.
    void ensureScheduled(const std::string& channel_id,
                         std::time_t from, int horizon_hours,
                         int seed = -1);

    // Mark the earliest matching scheduled entry as aired.
    void notifyPlayed(const std::string& channel_id, const std::string& item_id);

    // XMLTV XML for all channels. Calls ensureScheduled() then reads the cache.
    std::string generateXMLTV(int horizon_hours = 24);

    // M3U channel list. Stream URLs point to base_url/channels/{id}/stream.
    std::string generateM3U(const std::string& base_url);

private:
    static std::string xmlEscape(const std::string& s);
    static std::string fmtXMLTVTime(std::time_t t);

    Database&    db_;
    RuleEngine&  engine_;
};
