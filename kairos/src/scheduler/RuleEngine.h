#pragma once
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "../model/Block.h"
#include "../model/Episode.h"
#include "../model/Movie.h"

class Database;

struct ScheduledItem {
    std::string item_type;        // "episode" | "movie"
    std::string item_id;
    std::string file_path;
    int64_t     duration_ms         = 0;
    std::string title;
    std::string show_title;
    std::string show_id;
    int         season              = 0;
    int         episode_num         = 0;
    std::string channel_id;
    std::string block_id;
    int64_t     wall_clock_start_ms = 0;
    int64_t     wall_clock_end_ms   = 0;
    // SimState snapshot after this item is scheduled; used by EPGMaterializer
    // to resume extension from exactly the right cursor position.
    std::string cursor_json         = "{}";
};

class RuleEngine {
public:
    // Exposed for SimState JSON helpers in RuleEngine.cpp.
    struct SimState {
        std::unordered_map<std::string, int> show_pos;  // scope:scope_id:show_id → ep index
        std::unordered_map<std::string, int> block_rr;  // block_id → content index
    };

    explicit RuleEngine(Database& db);

    // Active block for channel at wall-clock time t (UTC).
    std::optional<Block> resolveBlock(const std::string& channel_id, std::time_t t);

    // Next item from a block (peek only — does not advance cursor).
    std::optional<ScheduledItem> nextItem(const std::string& channel_id,
                                           const Block& block);

    // Record playback completion: inserts play_history and advances cursor.
    void markPlayed(const std::string& channel_id, const std::string& block_id,
                    const std::string& item_type, const std::string& item_id,
                    int64_t duration_ms);

    // Forward EPG projection — read-only, does not touch DB.
    // initial_cursor_json: SimState snapshot from the last scheduled_program row;
    // pass "{}" (or omit) to seed from DB (media_cursor / block_state).
    // seed >= 0: deterministic mode — all cursor positions initialised from seed
    //            instead of DB state, so the same seed always yields the same schedule.
    std::vector<ScheduledItem> project(const std::string& channel_id,
                                        std::time_t start, int horizon_hours,
                                        const std::string& initial_cursor_json = "{}",
                                        int seed = -1);

    // Load all blocks for a channel with their content.
    std::vector<Block> loadBlocks(const std::string& channel_id);

private:
    std::vector<Episode> getEpisodes(const std::string& show_id, std::optional<int> season);
    std::optional<Movie> getMovie(const std::string& movie_id);
    std::string          showTitle(const std::string& show_id);

    int  readCursorPos(const std::string& content_type, const std::string& content_id,
                       const std::string& scope, const std::string& scope_id);
    void writeCursorPos(const std::string& content_type, const std::string& content_id,
                        const std::string& scope, const std::string& scope_id,
                        int pos, const std::string& episode_id = "");

    int  readBlockRR(const std::string& block_id, const std::string& channel_id);
    void writeBlockRR(const std::string& block_id, const std::string& channel_id, int pos);

    static std::string scopeStr(const Block& b);
    static std::string scopeId(const Block& b, const std::string& channel_id);

    // Like nextItem but operates on a mutable SimState (for project()).
    // seed >= 0: when initialising a cursor key not yet in state, use seed % size
    //            instead of reading the DB cursor.
    std::optional<ScheduledItem> nextItemSim(const std::string& channel_id,
                                              const Block& block, SimState& state,
                                              int seed = -1);

    std::optional<Block> resolveFromList(const std::vector<Block>& blocks, std::time_t t);

    Database& db_;
};
