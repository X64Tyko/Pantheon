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
    // filler_records_out: filler picks (fallback filler, inter-episode alignment
    // filler, and gap filler) are buffered separately from play_records_out so
    // EPGMaterializer::commit() can persist them to filler_play_history — kept
    // apart from play_history so filler plays never influence content-side
    // rerun/smart cooldown (getHotMovieIds/getHotEpisodeIds).
    std::vector<ScheduledItem> project(const std::string& channel_id,
                                        std::time_t start, int horizon_hours,
                                        CursorState& state,
                                        Xoshiro256& rng,
                                        std::map<std::time_t, std::string>* anchors_out = nullptr,
                                        std::vector<PlayRecord>* play_records_out = nullptr,
                                        std::vector<PlayRecord>* filler_records_out = nullptr);
	void scheduleBlockWindows(std::vector<Block>& blocks);

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

    // True if `entry`'s show has ever actually aired (real play history — DB or this
    // pass's own pass_records), independent of any local block cursor. The only thing
    // no_history_behavior governs (see NoHistoryBehavior): Exclude uses this to filter
    // a show out of selection entirely; once true, a show plays the same way regardless
    // of no_history_behavior.
    bool hasRealHistory(const std::string& channel_id, const Block& block,
                        const BlockContent& entry, std::time_t before_time,
                        const std::vector<PlayRecord>& pass_records);

    // Seeds or advances one show's cursor and returns its next item — the single place
    // that owns show playback: sequential resume-from-history (or a simulated pass from
    // episode 0 for Normal with zero history), then free-random once a full pass
    // completes (Fallback skips the sequential phase and starts free-random immediately).
    // Free-random picks respect Advancement::Smart cooldown filtering; nothing is
    // persisted for them since every pick is independent. Returns nullopt only if the
    // show has no episodes at all.
    std::optional<ScheduledItem> advanceShowCursor(const std::string& channel_id, const Block& block,
                                                    const BlockContent& entry, std::time_t before_time,
                                                    CursorState& state,
                                                    const std::vector<PlayRecord>& pass_records,
                                                    Xoshiro256& rng);

    // Selects the content index to play for this call and updates block position state.
    // For Exclude-mode rerun blocks, ineligible shows (hasRealHistory == false) are
    // filtered out of the candidate pool before the weighted draw — not retried after.
    // Does not touch episode-level cursors; that's advanceAndGet/advanceShowCursor's job
    // once the index is decided. Returns -1 only when Exclude mode finds nothing eligible.
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
        std::vector<PlayRecord>              filler_records;

        // Persisted (DB) filler_play_history is fixed for the whole pass — only
        // its content_.getLastPlayedMap() *query* is expensive (a full-table
        // GROUP BY that can be tens of thousands of rows for a long-lived
        // channel). Caching it here turns pickFillerSim's "sized" cooldown
        // lookup, previously re-querying on every single filler pick, into one
        // DB round trip for the whole generate() call — computed once (the
        // std::optional just marks "already computed"), never invalidated
        // mid-pass, since before_time only ever moves further past "now" (and
        // thus further past every already-persisted row) as pass.t advances —
        // see pickFillerSim.
        std::optional<std::time_t> filler_history_cached_before;
        std::unordered_map<std::string, int64_t> filler_history_cache;

        // Same rationale as filler_history_cache: a filler source's own item list
        // (e.g. a show's episodes) can't change mid-pass, but pickFillerSim was
        // reloading it from DB on every single pick — round-robin between just a
        // couple of sources means the same source gets re-fetched hundreds of
        // times per day of projection. Keyed by "content_type:content_id:season".
        std::unordered_map<std::string, std::vector<FillerItem>> filler_items_cache;
    };

    // Projection core.
    //
    // Windows (block.schedule_windows, built once per project() call by
    // scheduleBlockWindows) already encode priority, preemption, and
    // late_start_mins grace as plain time ranges — including deliberate overlap
    // where a lower-priority block's window resumes exactly at a preemptor's
    // (grace-extended) cut point. So the dispatch loop needs no preemption
    // bookkeeping of its own: it just repeatedly asks "which block owns pass.t
    // right now", has it schedule one program bounded by its own window_end, and
    // asks again. The overlap is what lets an in-flight lower-priority item finish
    // naturally before the next resolve picks up the higher-priority block.
    //
    // week_blocks is a fresh copy of ctx.blocks (with schedule_windows already
    // computed) made once per week in project() — BlockWindow::prog_count/
    // intro_played/exhausted are mutated directly on it as the week is scheduled,
    // and the copy is simply discarded at week's end, so nothing needs manual
    // resetting. Pieces sharing one occurrence_start (see BlockWindow) are kept in
    // sync by scheduleBlockStep as it mutates them, since they describe one
    // occurrence, not independent pieces.
    //
    // resolveActiveWindow: highest-priority block whose schedule_windows covers
    //   `t` (as a week-relative offset from week_start) and isn't exhausted.
    //   nullopt when nothing covers `t` (a genuine gap).
    //
    // scheduleBlockStep: schedules exactly one program (plus surrounding
    //   intro/interstitial/outro/alignment filler) for `block` starting at
    //   pass.t, advancing pass.t by however long that took. Knows its own
    //   window (`w.window_end`, converted to absolute via `window_end`) — if the
    //   next-selected item wouldn't fit before it, the selection is rolled back
    //   and sized filler plays instead (this is also what inter-program filler
    //   will need: how much room is left in the window). If nothing fits at all,
    //   marks `w` exhausted immediately rather than leaving a timed gap. Returns
    //   true if this call exhausted the occurrence (program_count hit, or nothing
    //   left to schedule).
    //
    // projectWeek: dispatch for one week. Loops resolveActiveWindow + scheduleBlockStep
    //   directly on week_blocks; on a gap, fills forward to the next window start
    //   with channel filler. Exits when pass.t >= week_end.
    struct ActiveWindow { Block* block; BlockWindow* window; };

    std::optional<ActiveWindow> resolveActiveWindow(std::vector<Block>& week_blocks,
                                                     std::time_t week_start,
                                                     std::time_t t);

    bool scheduleBlockStep(const ProjectContext& ctx,
                           Block& block,
                           BlockWindow& w,
                           std::time_t window_end,
                           ProjectPassState& pass);

    // Timeslot occurrences fill their whole window in one call (a fixed slot grid,
    // not exhaustible content) rather than one program at a time — window_end is
    // already grace-adjusted by scheduleBlockWindows.
    bool scheduleTimeslotBlock(const ProjectContext& ctx,
                               const Block& block,
                               std::time_t window_end,
                               ProjectPassState& pass);

    // Resolves the next item for one timeslot slot from its own queue (NOT
    // block.content — a timeslot slot's programming lives in TimeslotSlot::queue,
    // keyed by its own SlotCursor). Returns nullopt when the queue is empty or
    // every entry is pre-premiere-gated with no usable fallback, in which case
    // the caller falls through to filler exactly like an empty regular block.
    // Mutates `sc` (queue/episode position) in place on success.
    std::optional<ScheduledItem> pickTimeslotItem(const std::string& channel_id,
                                                   const Block& block,
                                                   const TimeslotSlot& slot,
                                                   SlotCursor& sc,
                                                   std::time_t at,
                                                   const std::string& tz);

    // Fill pass.t → target with sized filler; advance pass.t to target if no filler fits.
    void fillToTime(const ProjectContext& ctx,
                    const Block& block,
                    std::time_t target,
                    ProjectPassState& pass);

    void projectWeek(const ProjectContext& ctx,
                     std::vector<Block>& week_blocks,
                     std::time_t week_start,
                     std::time_t week_end,
                     ProjectPassState& pass);

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
    // history_cache/history_cached_before: caller's ProjectPassState fields (see there for why).
    std::optional<ScheduledItem> pickFillerSim(const std::string& channel_id,
                                               const Block& block,
                                               const std::vector<BlockFillerEntry>& pool,
                                               int64_t max_ms,
                                               CursorState& state,
                                               Xoshiro256& rng,
                                               const std::vector<PlayRecord>& pass_records,
                                               std::time_t before_time,
                                               std::optional<std::time_t>& history_cached_before,
                                               std::unordered_map<std::string, int64_t>& history_cache,
                                               std::unordered_map<std::string, std::vector<FillerItem>>& items_cache);

    Database&         db_;
    BlockRepository   blocks_;
    ContentRepository content_;
};
