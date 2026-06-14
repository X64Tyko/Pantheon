#pragma once
#include <ctime>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "../model/Block.h"
#include "../model/Episode.h"
#include "../model/Movie.h"

class Database;

struct ScheduledItem {
    std::string item_type;        // "episode" | "movie" | "filler" (merged preview block)
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
    bool        is_filler           = false; // true for items from pickFillerSim
};

class RuleEngine {
public:
    // Exposed for SimState JSON helpers in RuleEngine.cpp.
    struct SimState {
        std::unordered_map<std::string, int> show_pos;    // scope:scope_id:show_id → ep index
        std::unordered_map<std::string, int> block_rr;    // block_id → content index (non-rerun RR)
        std::unordered_map<std::string, int> rerun_sel;   // block_id → selected content position (rerun)
        std::unordered_map<std::string, int> rerun_runs;  // block_id → runs_remaining
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
    std::vector<Episode> getPlayedEpisodes(const std::string& show_id,
                                            const std::string& channel_id,
                                            std::optional<int> season);
    // Like getPlayedEpisodes but excludes the most-recently-played smart_pct% of the pool.
    std::vector<Episode> getPlayedEpisodesWithCooldown(const std::string& show_id,
                                                        const std::string& channel_id,
                                                        std::optional<int> season,
                                                        int smart_pct);
    std::optional<Movie> getMovie(const std::string& movie_id);
    std::string          showTitle(const std::string& show_id);

    // Weighted random selection of a content-item index from a block's content list.
    static int selectWeighted(const Block& block, std::mt19937_64& rng);

    // Given an episode index in eps, snap back to Part 1 of its multipart group (if any).
    int snapToGroupStart(const std::string& episode_id,
                         const std::vector<Episode>& eps) const;

    // Shuffle helpers.
    static std::vector<int> shufflePermutation(const std::string& seed_str, int n);

    // Rerun-mode helpers: read/write the selected content position and runs_remaining.
    int  readRunsRemaining(const std::string& block_id, const std::string& channel_id);
    void writeRerunState(const std::string& block_id, const std::string& channel_id,
                         int content_pos, int runs_remaining);

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

    // Returns the IANA timezone name for a channel (e.g. "America/Denver"), or "UTC".
    std::string channelTimezone(const std::string& channel_id);

    std::optional<Block> resolveFromList(const std::vector<Block>& blocks, std::time_t t,
                                         const std::string& tz = "UTC");

    // Pick one filler clip from the effective pool, advancing SimState cursors.
    // max_ms > 0: "sized" advancement will reject clips longer than this.
    std::optional<ScheduledItem> pickFillerSim(const std::string& channel_id,
                                               const Block& block,
                                               const std::vector<BlockFillerEntry>& pool,
                                               int64_t max_ms,
                                               SimState& state,
                                               int seed);

    Database& db_;
};
