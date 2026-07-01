#pragma once
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "../db/BlockRepository.h"
#include "../db/ContentRepository.h"
#include "../model/Block.h"
#include "../model/Episode.h"
#include "../model/Movie.h"
#include "CursorState.h"
#include "Rng.h"

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

    // Forward EPG projection. Reads and writes cursor state entirely through `state`;
    // no DB cursor reads or writes occur during projection. The caller (EPGMaterializer)
    // loads state before calling and applies it to DB afterward — or discards it for
    // preview / on_play modes.
    //
    // play_history (is_scheduled=1) rows are buffered in play_records_out during
    // projection and written to DB by EPGMaterializer::commit(), keeping project()
    // free of DB writes.
    //
    // rng: caller-owned. Pass the same instance across successive calls to preserve
    //      RNG continuity when the ensureScheduled loop extends a schedule.
    // anchors_out: if non-null, receives {week_monday_ts -> JSON snapshot} for each
    //      Monday midnight boundary crossed. JSON contains rng state + serialized
    //      CursorState for deterministic weekly rebuilds.
    std::vector<ScheduledItem> project(const std::string& channel_id,
                                        std::time_t start, int horizon_hours,
                                        CursorState& state,
                                        Xoshiro256& rng,
                                        std::map<std::time_t, std::string>* anchors_out = nullptr,
                                        std::vector<PlayRecord>* play_records_out = nullptr);

    // Convenience overload for callers that don't manage CursorState externally
    // (tests, one-shot previews). Starts with a fresh empty state and discards it.
    std::vector<ScheduledItem> project(const std::string& channel_id,
                                        std::time_t start, int horizon_hours,
                                        Xoshiro256& rng) {
        CursorState state;
        return project(channel_id, start, horizon_hours, state, rng);
    }

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
    static int selectWeighted(const Block& block, Xoshiro256& rng);
    // Like selectWeighted but skips exclude_idx. Falls back to selectWeighted if all excluded.
    static int selectWeightedExcluding(const Block& block, int exclude_idx, Xoshiro256& rng);

    // Weighted content-entry selection with movie-level recency cooldown. Excludes the
    // n*smart_pct/100 most-recently-played movie entries from the weighted draw.
    // Only applies when every block content entry is a movie; mixed blocks fall back to
    // selectWeighted. (Show content uses smart_pct at the episode-pool level via
    // smartShufflePool; the block-selection level always sees the full show list.)
    int selectWeightedSmartCooldown(const Block& block, const std::string& channel_id,
                                    int smart_pct, std::time_t before_time,
                                    const std::vector<PlayRecord>& play_records,
                                    Xoshiro256& rng);

    // For SmartShuffle show blocks: filters `all` to exclude the most recently played
    // smart_pct% of episodes. Falls back to `all` if every episode is hot.
    std::vector<Episode> smartShufflePool(const std::vector<Episode>& all,
                                          const std::string& show_id,
                                          const std::string& channel_id,
                                          int smart_pct, std::time_t before_time,
                                          const std::vector<PlayRecord>& play_records);

    // Given an episode_id, snap back to Part 1 of its multipart group (if any).
    int snapToGroupStart(const std::string& episode_id, const std::vector<Episode>& eps);

    // Produces a deterministic shuffle permutation from a string seed.
    // The seed-string design is intentional: same seed always produces the same order
    // regardless of live RNG state, enabling reproducible shuffles across projections.
    static std::vector<int> shufflePermutation(const std::string& seed_str, int n);

    // Like shufflePermutation but keeps multipart episodes consecutive as atomic units.
    std::vector<int> groupedShufflePermutation(const std::string& seed_str,
                                               const std::vector<Episode>& eps);

    // Get the aired episodes for a show, prioritizing memory cursors.
    std::vector<Episode> getAvailableEpisodesForShow(const std::string& channel_id, const Block& block,
                                                     const BlockContent& entry, std::time_t before_time,
                                                     const CursorState& state, const std::vector<PlayRecord>& pass_records);

    // ── Rerun-block seed cursors (once-per-block-per-day init in projectDay) ──────
    //
    // Decides what starting cursor, if any, to seed for one show content entry that
    // has no persisted block position yet. `played` is that show's real play history
    // (episode_order applied, already scoped/cooled by the caller); `all` is its full
    // episode catalog. Returns nullopt to mean "leave this show genuinely unseeded" —
    // callers must never fabricate a cursor to paper over that.
    //
    // Split one function per no_history_behavior (rather than a single branchy block)
    // so a new behavior is one new function + one switch case, without touching the
    // others or the shared real-history math.
    struct SeedCursor { std::string content_type; int position = 0; std::string episode_id; };

    // Shared by every behavior: if `played` is non-empty, resume first-run right after
    // it (or flip to full rerun mode once `played` covers the whole catalog). Returns
    // nullopt when there is no real history to seed from.
    std::optional<SeedCursor> seedFromRealHistory(const std::vector<Episode>& played,
                                                  const std::vector<Episode>& all,
                                                  bool snap_to_group_start, Xoshiro256& rng);

    // Normal: optimistic either way — real history resumes first-run; no history at
    // all still gets a random first-run starting point (channel seed diversity).
    std::optional<SeedCursor> getContentNormalHistory(const std::vector<Episode>& played,
                                                       const std::vector<Episode>& all,
                                                       bool snap_to_group_start, Xoshiro256& rng);
    // FallbackAll: no real history means the whole catalog is already a legitimate
    // rerun pool, so seed straight into rerun mode instead of a first-run position.
    std::optional<SeedCursor> getContentFallbackHistory(const std::vector<Episode>& played,
                                                         const std::vector<Episode>& all,
                                                         bool snap_to_group_start, Xoshiro256& rng);
    // Skip: no real history means this slot should be skipped entirely — never
    // fabricate a cursor; leave the show unseeded so the empty-pool path takes over.
    std::optional<SeedCursor> getContentSkipHistory(const std::vector<Episode>& played,
                                                     const std::vector<Episode>& all,
                                                     bool snap_to_group_start, Xoshiro256& rng);
    // Exclude: same contract as Skip — no real history must stay unseeded so
    // pickNextContent's weighted-selection pool can leave this show out.
    std::optional<SeedCursor> getContentExcludeHistory(const std::vector<Episode>& played,
                                                        const std::vector<Episode>& all,
                                                        bool snap_to_group_start, Xoshiro256& rng);

    // Select the next episode from a pool based on advancement and history behavior.
    std::optional<ScheduledItem> selectNextEpisode(const std::string& channel_id, const Block& block,
                                                   const BlockContent& entry, const std::vector<Episode>& all_eps,
                                                   const std::vector<Episode>& rerun_pool, CursorState& state);
    // Selects the content index to play for this call, updates block position state.
    // Returns -1 only when Exclude mode finds no eligible content.
    int pickNextContent(const std::string& channel_id, const Block& block,
                        std::time_t before_time, CursorState& state,
                        const std::vector<PlayRecord>& pass_records, Xoshiro256& rng);

    // Episode/item advancement for a pre-selected content entry. content_idx is the
    // index into block.content returned by pickNextContent. Returns nullopt when no
    // item is available (empty pool, empty show, etc.) without advancing.
    std::optional<ScheduledItem> advanceAndGet(const std::string& channel_id,
                                               const Block& block,
                                               int content_idx,
                                               std::time_t before_time,
                                               CursorState& state,
                                               const std::vector<PlayRecord>& pass_records,
                                               Xoshiro256& rng);

    static std::string scopeStr(const Block& b);
    static std::string scopeId(const Block& b, const std::string& channel_id);

    std::string channelTimezone(const std::string& channel_id);
    std::string channelAdvanceMode(const std::string& channel_id);

    // Channel bumper entry used for "between" injection mode.
    struct BetweenBumper { int id; std::string ct, cid; int every_n; };

    // Constant data for the full projection pass. Constructed once in project()
    // and passed by const-ref through all sub-calls.
    struct ProjectContext {
        const std::string&                   channel_id;
        const std::vector<Block>&            blocks;
        const std::vector<BlockFillerEntry>& channel_filler;
        const std::vector<BetweenBumper>&    between_bumpers;
        const std::string&                   tz;
        std::time_t                          proj_start;
        int                                  rerun_min_time_mins = 0;
        std::vector<ScheduledItem>&          result;
        CursorState&                         state;
        Xoshiro256&                          rng;
        std::map<std::time_t,std::string>*   anchors_out;
        // Synthetic block (empty block_id, channel's default_filler_selection) used to
        // materialize channel_filler into stretches no real block covers — keeps those
        // gaps inside the deterministic engine (rotation, smart_pct cooldown) instead of
        // leaving a hole for the live "/now" path to fill with an ungoverned random pick.
        const Block&                         gap_block;
    };

    // Mutable state that survives across day boundaries within a projection pass.
    struct ProjectPassState {
        std::time_t                          t                  = 0;
        std::time_t                          anchor_next_monday = 0;
        std::string                          prev_block_id;
        std::string                          last_show_id;
        std::unordered_map<std::string, int> transition_counts;
        int                                  channel_prog_count = 0;
        std::vector<PlayRecord>              play_records;
    };

    // Three-layer projection core.
    //
    // scheduleBlock: item loop for one block occurrence within [pass.t, window_end).
    //   Returns true if block hit its program_count (exhausted) before window_end.
    //   Items that would span window_end are rolled back. Day boundary (day_start)
    //   is used for block end_time arithmetic; items ARE allowed to complete past it.
    //
    // projectDay: dispatch for one calendar day. Owns a local exhausted set (reset
    //   per day). Resolves the active block at pass.t, computes its preemption window,
    //   calls scheduleBlock. On exhaustion the block is removed from the active set
    //   and the loop continues — no recursion needed. Exits when pass.t >= day_end
    //   OR pass.t >= t_end (whichever comes first for dispatch purposes).
    bool scheduleBlock(const ProjectContext& ctx,
                       const Block& block,
                       std::time_t window_end,
                       int window_late_start_mins,
                       bool first_entry,
                       std::time_t day_start,
                       ProjectPassState& pass);

    // Timeslot-specific scheduling helpers.
    bool scheduleTimeslotBlock(const ProjectContext& ctx,
                               const Block& block,
                               std::time_t window_end,
                               int window_late_start_mins,
                               std::time_t day_start,
                               ProjectPassState& pass);

    // Fill pass.t → target with sized filler; advance pass.t to target if no filler fits.
    void fillToTime(const ProjectContext& ctx,
                    const Block& block,
                    std::time_t target,
                    ProjectPassState& pass);

    void projectDay(const ProjectContext& ctx,
                    std::time_t day_start,
                    std::time_t day_end,
                    int day_mask_bit,
                    ProjectPassState& pass,
                    std::time_t t_end);

    std::optional<Block> resolveFromList(const std::vector<Block>& blocks, std::time_t t,
                                         const std::string& tz = "UTC");

    // Resolves (content_type, content_id, position) to a ScheduledItem. position is
    // modulo-indexed for show/playlist; ignored for episode/movie.
    std::optional<ScheduledItem> pickFromSource(const std::string& channel_id,
                                                const std::string& content_type,
                                                const std::string& content_id,
                                                int position);

    std::optional<ScheduledItem> pickBumperItem(const std::string& channel_id,
                                                const std::string& content_type,
                                                const std::string& content_id,
                                                const std::string& scope_id,
                                                CursorState& state);
    void advanceBumperCursor(const std::string& content_type,
                             const std::string& content_id,
                             const std::string& scope_id,
                             CursorState& state);

    bool scheduleBumperItem(const std::string& channel_id,
                            const std::string& block_id,
                            const std::string& content_type,
                            const std::string& content_id,
                            const std::string& scope_id,
                            std::vector<ScheduledItem>& result,
                            std::time_t& t,
                            CursorState& state);

    // Pick one filler clip from the effective pool, advancing filler positions in state.
    // max_ms > 0: "sized" advancement rejects clips longer than this.
    std::optional<ScheduledItem> pickFillerSim(const std::string& channel_id,
                                               const Block& block,
                                               const std::vector<BlockFillerEntry>& pool,
                                               int64_t max_ms,
                                               CursorState& state,
                                               Xoshiro256& rng,
                                               const std::vector<PlayRecord>& pass_records,
                                               std::time_t before_time = 0);

    Database&         db_;
    BlockRepository   blocks_;
    ContentRepository content_;
};
