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
    // Filler-only sim state — used by pickFillerSim() within a single project() call.
    // Content cursor state (show positions, rerun selection) is now entirely in the DB.
    struct SimState {
        std::unordered_map<std::string, int> show_pos;    // fl_rr:block_id, fl_pos:list_id:block_id
    };

    explicit RuleEngine(Database& db);

    // Active block for channel at wall-clock time t (UTC).
    std::optional<Block> resolveBlock(const std::string& channel_id, std::time_t t);

    // Next item from a block (peek only — does not advance cursor).
    // before_time: only episodes with aired_at < before_time are valid rerun candidates.
    std::optional<ScheduledItem> nextItem(const std::string& channel_id,
                                           const Block& block,
                                           std::time_t before_time);

    // Record playback completion: inserts play_history and advances cursor.
    void markPlayed(const std::string& channel_id, const std::string& block_id,
                    const std::string& item_type, const std::string& item_id,
                    int64_t duration_ms);

    // Forward EPG projection — writes play_history (is_scheduled=1) and advances
    // DB cursors as it schedules each item. Must be called within a transaction or
    // savepoint managed by the caller (EPGMaterializer::ensureScheduled).
    // seed >= 0: randomise the starting cursor positions for each block's first entry.
    std::vector<ScheduledItem> project(const std::string& channel_id,
                                        std::time_t start, int horizon_hours,
                                        int seed = -1);

    // Load all blocks for a channel with their content.
    std::vector<Block> loadBlocks(const std::string& channel_id);

private:
    std::vector<Episode> getEpisodes(const std::string& show_id, std::optional<int> season,
                                      bool include_specials = false,
                                      const std::string& episode_order = "season");
    // Only episodes with aired_at < before_time are returned (ensures true reruns).
    std::vector<Episode> getPlayedEpisodes(const std::string& show_id,
                                            const std::string& channel_id,
                                            std::optional<int> season,
                                            std::time_t before_time,
                                            bool global_scope = false,
                                            bool include_specials = false,
                                            const std::string& episode_order = "season");
    // Like getPlayedEpisodes but excludes the most-recently-played smart_pct% of the pool.
    std::vector<Episode> getPlayedEpisodesWithCooldown(const std::string& show_id,
                                                        const std::string& channel_id,
                                                        std::optional<int> season,
                                                        int smart_pct,
                                                        std::time_t before_time,
                                                        bool global_scope = false,
                                                        bool include_specials = false);
    std::optional<Movie>         getMovie(const std::string& movie_id);
    std::optional<ScheduledItem> episodeById(const std::string& episode_id);
    // Returns (item_type, item_id) pairs from a playlist or filler_list in order.
    std::vector<std::pair<std::string, std::string>>
        loadListItems(const std::string& content_type, const std::string& content_id);
    std::string          showTitle(const std::string& show_id);

    // Playlist show_collection helpers.
    std::string              getPlaylistMode(const std::string& playlist_id);
    std::vector<std::string> getPlaylistShows(const std::string& playlist_id);
    std::vector<Episode>     getPlaylistShowEpisodes(const std::string& playlist_id,
                                                      const std::string& show_id);

    // Weighted random selection of a content-item index from a block's content list.
    static int selectWeighted(const Block& block, std::mt19937_64& rng);

    // Given an episode index in eps, snap back to Part 1 of its multipart group (if any).
    int snapToGroupStart(const std::string& episode_id,
                         const std::vector<Episode>& eps) const;

    // Shuffle helpers.
    static std::vector<int> shufflePermutation(const std::string& seed_str, int n);

    // Like shufflePermutation but keeps multipart episodes (Part 1/2/…) consecutive.
    // Groups are shuffled as atomic units; within each group parts retain their part_num order.
    std::vector<int> groupedShufflePermutation(const std::string& seed_str,
                                               const std::vector<Episode>& eps) const;

    // Rerun-mode helpers: read/write the selected content position and runs_remaining.
    int  readRunsRemaining(const std::string& block_id, const std::string& channel_id);
    int  readConsecutiveCount(const std::string& block_id, const std::string& channel_id);
    void writeRerunState(const std::string& block_id, const std::string& channel_id,
                         int content_pos, int runs_remaining, int consecutive_count = 0);

    int  readCursorPos(const std::string& content_type, const std::string& content_id,
                       const std::string& scope, const std::string& scope_id);
    void writeCursorPos(const std::string& content_type, const std::string& content_id,
                        const std::string& scope, const std::string& scope_id,
                        int pos, const std::string& episode_id = "");

    int  readBlockRR(const std::string& block_id, const std::string& channel_id);
    void writeBlockRR(const std::string& block_id, const std::string& channel_id, int pos);

    static std::string scopeStr(const Block& b);
    static std::string scopeId(const Block& b, const std::string& channel_id);

    // Advance DB cursors after scheduling or confirming a play of one item from `block`.
    // before_time: same semantics as getPlayedEpisodes — rerun pool filtered to aired_at < before_time.
    void advanceCursors(const std::string& channel_id, const Block& block,
                        std::time_t before_time);

    // Returns the IANA timezone name for a channel (e.g. "America/Denver"), or "UTC".
    std::string channelTimezone(const std::string& channel_id);

    // Returns the advance mode for a channel: "scheduled" or "on_play".
    std::string channelAdvanceMode(const std::string& channel_id);

    std::optional<Block> resolveFromList(const std::vector<Block>& blocks, std::time_t t,
                                         const std::string& tz = "UTC");

    // Pick one filler clip from the effective pool, advancing SimState cursors.
    // max_ms > 0: "sized" advancement rejects clips longer than this and picks
    //             the least recently played clip that fits (recency from play_history).
    // before_time > 0: recency query includes only plays with aired_at <= before_time.
    std::optional<ScheduledItem> pickFillerSim(const std::string& channel_id,
                                               const Block& block,
                                               const std::vector<BlockFillerEntry>& pool,
                                               int64_t max_ms,
                                               SimState& state,
                                               int seed,
                                               std::time_t before_time = 0);

    Database& db_;
};
