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

    // Monday 00:00, in channel_id's own timezone, for the week containing t.
    // EPGMaterializer's anchor keys (generate()'s week_monday,
    // checkAnchorDivergence()'s week_monday/prev_monday) MUST use this exact
    // function, not naive UTC-calendar-week arithmetic — project() below builds
    // its week-walk (and week-boundary anchor captures) from the identical
    // timezone-aware definition, and for any channel not on UTC a naive week
    // boundary can land days away from where this one falls, silently keying
    // an anchor somewhere a later lookup can never find it again.
    std::time_t weekMondayForChannel(const std::string& channel_id, std::time_t t);

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
    // rerun/smart cooldown (CursorState::recentPlays).
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
    std::optional<Movie>         getMovie(const std::string& movie_id);
    std::optional<ScheduledItem> episodeById(const std::string& episode_id);
    // Returns (item_type, item_id) pairs from a playlist or filler_list in order.
    std::vector<std::pair<std::string, std::string>>
        loadListItems(const std::string& content_type, const std::string& content_id);
    std::string          showTitle(const std::string& show_id);

    // ── In-memory content cache ────────────────────────────────────────────────
    // A projection pass calls getEpisodes/getMovie/episodeById/loadListItems/
    // showTitle many times for the same show/movie/list (once per occurrence
    // across the whole horizon) — these memoize each on first touch per
    // project() call, same pattern as filler_items_cache below. This is what
    // makes project() do at most one DB read per distinct piece of content per
    // call, instead of one per pick.
    struct ContentCache {
        std::unordered_map<std::string, std::vector<Episode>>              episodes;      // key: showCacheKey(...)
        std::unordered_map<std::string, std::optional<Movie>>              movies;        // key: movie_id
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> list_items; // key: content_type+":"+content_id
        std::unordered_map<std::string, std::optional<ScheduledItem>>      episode_by_id; // key: episode_id
        std::unordered_map<std::string, std::string>                      show_titles;   // key: show_id
        std::unordered_map<std::string, std::vector<std::string>>          playlist_shows;     // key: playlist_id
        std::unordered_map<std::string, std::vector<Episode>>              playlist_show_eps;  // key: playlist_id+":"+show_id
        std::unordered_map<std::string, int>                               playlist_item_counts; // key: playlist_id
    };
    static std::string showCacheKey(const std::string& show_id, std::optional<int> season,
                                     bool include_specials, const std::string& episode_order);
    const std::vector<Episode>& getEpisodesCached(ContentCache& cache, const std::string& show_id,
                                                   std::optional<int> season,
                                                   bool include_specials,
                                                   const std::string& episode_order);
    const std::optional<Movie>& getMovieCached(ContentCache& cache, const std::string& movie_id);
    const std::vector<std::pair<std::string, std::string>>&
        getListItemsCached(ContentCache& cache, const std::string& content_type,
                           const std::string& content_id);
    const std::optional<ScheduledItem>& episodeByIdCached(ContentCache& cache,
                                                            const std::string& episode_id);
    std::string showTitleCached(ContentCache& cache, const std::string& show_id);
    const std::vector<std::string>& getPlaylistShowsCached(ContentCache& cache,
                                                             const std::string& playlist_id);
    const std::vector<Episode>& getPlaylistShowEpisodesCached(ContentCache& cache,
                                                                const std::string& playlist_id,
                                                                const std::string& show_id);
    int getPlaylistItemCountCached(ContentCache& cache, const std::string& playlist_id);

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
    // n*smart_pct/100 most-recently-played movie entries from the weighted draw, using
    // CursorState's in-anchor recency list (domain "movie:<channel_id>") — no DB read.
    // Only applies when every block content entry is a movie; mixed blocks fall back to
    // selectWeighted. (Show content uses smart_pct at the episode-pool level via
    // smartShufflePool; the block-selection level always sees the full show list.)
    int selectWeightedSmartCooldown(const Block& block, const std::string& channel_id,
                                    int smart_pct, CursorState& state, Xoshiro256& rng);

    // For SmartShuffle show blocks: filters `all` to exclude the most recently played
    // smart_pct% of episodes, using CursorState's in-anchor recency list (domain
    // "episode:<show_id>:<channel_id>") — no DB read. Falls back to `all` if every
    // episode is hot.
    std::vector<Episode> smartShufflePool(const std::vector<Episode>& all,
                                          const std::string& show_id,
                                          const std::string& channel_id,
                                          int smart_pct, CursorState& state);

    // Given an episode_id, snap back to Part 1 of its multipart group (if any).
    int snapToGroupStart(const std::string& episode_id, const std::vector<Episode>& eps);

    // Produces a deterministic shuffle permutation from a string seed.
    // The seed-string design is intentional: same seed always produces the same order
    // regardless of live RNG state, enabling reproducible shuffles across projections.
    static std::vector<int> shufflePermutation(const std::string& seed_str, int n);

    // True if `entry`'s show has a cursor at all yet — i.e. it has been picked at
    // least once before (purely from `state`; a pick earlier in this same pass
    // already updated state, so no separate pass_records check is needed). The
    // only thing no_history_behavior governs (see NoHistoryBehavior): Exclude uses
    // this to filter a show out of selection entirely; once true, a show plays the
    // same way regardless of no_history_behavior.
    bool hasRealHistory(const std::string& channel_id, const Block& block,
                        const BlockContent& entry, CursorState& state);

    // Natural-order (narrative-sequence) advancement result: which episode to play
    // now, and the watermark/ahead state to persist afterward. See advanceNatural().
    struct NaturalAdvance {
        int index = -1; // index into ordered_eps to play now; -1 if ordered_eps is empty
        std::string new_watermark_id;
        std::vector<std::string> new_ahead;
    };

    // Pure natural-order advancement, immune to ordered_eps being resorted/regrown
    // between calls (episodes backfilled into the library later, in any order).
    //
    // watermark_id: the last episode known to have aired in contiguous natural
    //   order ("" = nothing aired yet). ahead: episode ids that aired out of
    //   order, ahead of the watermark (e.g. episode 3 played because episode 2
    //   hadn't been scanned into the library yet).
    //
    // Picks the slot right after the watermark. If that episode id is already in
    // `ahead` (it already aired), absorbs it — advances the watermark through it
    // and moves on to the next slot — repeating until landing on something not
    // already aired; this is what lets a backfilled gap self-heal once the
    // missing episode shows up, instead of needing a full wraparound cycle to
    // reach it. If the natural next episode (by season/episode numbering) isn't
    // in the catalog yet, plays the next *available* one instead and records it
    // in `ahead` without moving the watermark, so the true next episode still
    // gets its turn once it backfills. Wrapping from the last episode back to the
    // first (starting a rerun cycle) is never treated as an out-of-order pick.
    static NaturalAdvance advanceNatural(const std::string& watermark_id,
                                         const std::vector<std::string>& ahead,
                                         const std::vector<Episode>& ordered_eps);

    // Seeds or advances one show's cursor and returns its next item — the single place
    // that owns rerun-mode show playback: natural-order resume via advanceNatural()
    // (or a simulated pass from episode 0 for Normal with zero history), then
    // free-random once a full pass completes (Fallback skips the natural-order phase
    // and starts free-random immediately). Free-random picks respect Advancement::Smart
    // cooldown filtering; nothing is persisted for them since every pick is independent.
    // Returns nullopt only if the show has no episodes at all.
    std::optional<ScheduledItem> advanceShowCursor(const std::string& channel_id, const Block& block,
                                                    const BlockContent& entry,
                                                    CursorState& state, Xoshiro256& rng,
                                                    ContentCache& cache);

    // Selects the content index to play for this call and updates block position state.
    // For Exclude-mode rerun blocks, ineligible shows (hasRealHistory == false) are
    // filtered out of the candidate pool before the weighted draw — not retried after.
    // Does not touch episode-level cursors; that's advanceAndGet/advanceShowCursor's job
    // once the index is decided. Returns -1 only when Exclude mode finds nothing eligible.
    int pickNextContent(const std::string& channel_id, const Block& block,
                        CursorState& state, Xoshiro256& rng);

    // Episode/item advancement for a pre-selected content entry. content_idx is the
    // index into block.content returned by pickNextContent. Returns nullopt when no
    // item is available (empty pool, empty show, etc.) without advancing.
    std::optional<ScheduledItem> advanceAndGet(const std::string& channel_id,
                                               const Block& block,
                                               int content_idx,
                                               CursorState& state,
                                               Xoshiro256& rng,
                                               ContentCache& cache);

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
        std::string                          prev_block_id;
        std::string                          last_show_id;
        std::unordered_map<std::string, int> transition_counts;
        int                                  channel_prog_count = 0;
        std::vector<PlayRecord>              play_records;
        std::vector<PlayRecord>              filler_records;

        // A filler source's own item list (e.g. a show's episodes) can't change
        // mid-pass, but pickFillerSim was reloading it from DB on every single
        // pick — round-robin between just a
        // couple of sources means the same source gets re-fetched hundreds of
        // times per day of projection. Keyed by "content_type:content_id:season".
        std::unordered_map<std::string, std::vector<FillerItem>> filler_items_cache;

        // Episodes/movies/list items for every show/movie/playlist touched during
        // this pass, memoized on first read — see ContentCache.
        ContentCache content_cache;
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
                                                   const std::string& tz,
                                                   ContentCache& cache);

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
                                                int position,
                                                ContentCache& cache);

    std::optional<ScheduledItem> pickBumperItem(const std::string& channel_id,
                                                const std::string& content_type,
                                                const std::string& content_id,
                                                const std::string& scope_id,
                                                CursorState& state,
                                                ContentCache& cache);
    void advanceBumperCursor(const std::string& content_type,
                             const std::string& content_id,
                             const std::string& scope_id,
                             CursorState& state,
                             ContentCache& cache);

    bool scheduleBumperItem(const std::string& channel_id,
                            const std::string& block_id,
                            const std::string& content_type,
                            const std::string& content_id,
                            const std::string& scope_id,
                            std::vector<ScheduledItem>& result,
                            std::time_t& t,
                            CursorState& state,
                            ContentCache& cache);

    // Pick one filler clip from the effective pool, advancing filler positions in state.
    // max_ms > 0: "sized" advancement rejects clips longer than this. Recency
    // (LRU for "sized", cooldown for shuffle/sequential) comes from
    // state.recentPlays("filler:"+channel_id) — no DB read.
    // items_cache: caller's ProjectPassState::filler_items_cache (see there for why).
    std::optional<ScheduledItem> pickFillerSim(const std::string& channel_id,
                                               const Block& block,
                                               const std::vector<BlockFillerEntry>& pool,
                                               int64_t max_ms,
                                               CursorState& state,
                                               Xoshiro256& rng,
                                               std::unordered_map<std::string, std::vector<FillerItem>>& items_cache,
                                               ContentCache& cache);

    Database&         db_;
    BlockRepository   blocks_;
    ContentRepository content_;
};
