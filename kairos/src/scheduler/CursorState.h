#pragma once
#include <ctime>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Database;

// Cursor for one timeslot slot: which queue item, and — for a "show" queue
// entry — which episode within it (watermark + out-of-order exceptions; see
// CursorEntry below for the same model applied to general show cursors).
struct SlotCursor {
    int         queue_pos = 0;
    std::string episode_watermark_id;
    std::vector<std::string> episode_ahead;
};

// A single media cursor entry.
//
// For content_type "show" / "show_rerun", position tracking is a watermark,
// not a raw index: episode_id is the last episode known to have aired in
// contiguous natural order, and ahead holds any episode ids that aired out
// of order beyond it (e.g. a later episode played because an earlier one
// hadn't been scanned into the library yet). This makes the cursor immune
// to the episode list being resorted/regrown between calls — see
// RuleEngine::advanceNatural(). Everything else (playlist/filler_list flat
// cursors) still uses plain integer `position`.
struct CursorEntry {
    std::string content_type;
    std::string content_id;
    std::string cursor_scope; // "global" | "channel" | "block"
    std::string scope_id;     // "" for global, channel_id, or block_id
    std::string episode_order; // "" for non-show types; disambiguates two blocks
                                // playing the same show in different orders
    int         position   = 0;
    std::string episode_id;   // "show": watermark id ("" = nothing aired yet)
    std::vector<std::string> ahead; // "show": ids aired out of order, ahead of the watermark
};

struct BlockPosition {
    int content_position  = 0; // index into block.content (replaces "blockrr")
    int runs_remaining    = 0;
    int consecutive_count = 0;
};

struct PlayRecord {
    std::string channel_id;
    std::string item_type;  // "episode" | "movie"
    std::string item_id;
    std::string show_id;    // populated for episodes; empty for movies
    std::string block_id;
    std::time_t aired_at = 0;
};

// In-memory snapshot of all mutable scheduling state for one channel's projection pass.
//
// Replaces per-item DB reads/writes for media_cursor, block_state, and SimState inside
// project(). The caller (EPGMaterializer) loads state from DB before calling project()
// and decides whether to apply the result back to DB afterward — or simply discard it
// (preview / on_play modes). No SAVEPOINTs needed for cursor state.
//
class CursorState {
public:
    // ── Media cursors ─────────────────────────────────────────────────────────
    // episode_order only matters for content_type "show"/"show_rerun" — pass ""
    // (the default) for movie/playlist/filler_list cursors, which don't use it.
    int  getCursorPos(const std::string& content_type, const std::string& content_id,
                      const std::string& scope, const std::string& scope_id,
                      const std::string& episode_order = "") const;
    // Returns the episode_id stored alongside the cursor position — for "show"
    // types this is the watermark (last contiguous aired episode), "" if none.
    std::string getCursorEpisodeId(const std::string& content_type, const std::string& content_id,
                                    const std::string& scope, const std::string& scope_id,
                                    const std::string& episode_order = "") const;
    // Episode ids aired out of order, ahead of the watermark ("show" types only).
    std::vector<std::string> getCursorAhead(const std::string& content_type, const std::string& content_id,
                                             const std::string& scope, const std::string& scope_id,
                                             const std::string& episode_order = "") const;
    void setCursorPos(const std::string& content_type, const std::string& content_id,
                      const std::string& scope, const std::string& scope_id,
                      int pos, const std::string& episode_id = "",
                      const std::string& episode_order = "",
                      const std::vector<std::string>& ahead = {});

    bool hasCursor(const std::string& content_type, const std::string& content_id,
                   const std::string& scope, const std::string& scope_id,
                   const std::string& episode_order = "") const;

    // ── Block state ───────────────────────────────────────────────────────────
    int  getContentPosition(const std::string& block_id) const;
    int  getRunsRemaining(const std::string& block_id) const;
    int  getConsecutiveCount(const std::string& block_id) const;
    bool hasBlockPosition(const std::string& block_id) const;

    // Sets all three block fields atomically (matches writeRerunState semantics).
    void setBlockPosition(const std::string& block_id,
                          int content_position, int runs_remaining, int consecutive_count);
    // Updates only content_position, preserving runs_remaining and consecutive_count.
    void setContentPosition(const std::string& block_id, int content_position);

    // ── Timeslot slot cursors ─────────────────────────────────────────────────
    SlotCursor  getSlotCursor(const std::string& slot_id) const;
    void        setSlotCursor(const std::string& slot_id, int queue_pos,
                              const std::string& episode_watermark_id,
                              const std::vector<std::string>& episode_ahead);
    bool        hasSlotCursor(const std::string& slot_id) const;
    const std::unordered_map<std::string, SlotCursor>& slotCursors() const { return slot_cursors_; }

    // ── Filler positions (absorbs SimState::show_pos) ─────────────────────────
    // Keys follow the existing SimState conventions:
    //   "fl_rr:<block_id>"                          — filler list round-robin position
    //   "fl_pos:<content_type>:<content_id>:<block_id>" — item position within a list
    int& fillerPos(const std::string& key);
    int  getFillerPos(const std::string& key) const;
    bool hasFillerPos(const std::string& key) const;

    // ── Smart-advancement cooldown recency ────────────────────────────────────
    // Replaces the old live ContentRepository::getHotMovieIds/getHotEpisodeIds
    // DB queries: a bounded, most-recent-first list of item ids per cooldown
    // domain, carried in the anchor instead of re-derived from play_history.
    // domain_key convention: "movie:<channel_id>" | "episode:<show_id>:<channel_id>".
    void recordRecentPlay(const std::string& domain_key, const std::string& item_id, std::time_t at);
    std::vector<std::pair<std::string, std::time_t>> recentPlays(const std::string& domain_key) const;

    // ── In-pass play records ──────────────────────────────────────────────────
    // Accumulates items scheduled during this projection pass.
    // Written to play_history (is_scheduled=1) by EPGMaterializer::commit(), not during project().
    void addPlayRecord(const std::string& channel_id, const std::string& item_type,
                       const std::string& item_id, const std::string& show_id,
                       const std::string& block_id, std::time_t aired_at);
    const std::vector<PlayRecord>& playRecords() const { return play_records_; }

    // ── DB I/O ────────────────────────────────────────────────────────────────

    // Loads channel-scoped and block-scoped cursors + block_state for channel_id.
    // Global-scoped cursors are also loaded so rerun pool queries see cross-channel
    // history; they are written back via targeted upserts (not delete-reinsert).
    static CursorState loadFromDB(Database& db, const std::string& channel_id);

    // Writes cursors and block_state back to DB. Channel-scoped and block-scoped
    // entries are deleted and reinserted; global entries are upserted individually.
    void applyToDB(Database& db, const std::string& channel_id) const;

    // Removes all channel-scoped/block-scoped cursors and block_state for a channel.
    // Used before a fresh projection or when clearing a channel's schedule.
    static void clearFromDB(Database& db, const std::string& channel_id);

    // ── Anchor serialization ──────────────────────────────────────────────────
    // Produces the {"cursors": [...], "block_states": [...], "slot_cursors": [...],
    // "filler_positions": [...], "recent_plays": [...]} object embedded in
    // anchor_hashes — everything that must survive a week boundary so project()
    // never needs to fall back to a live DB read for it.
    std::string serializeCursors() const;
    static CursorState deserializeCursors(const std::string& json_str);

private:
    static std::string cursorKey(const std::string& content_type, const std::string& content_id,
                                  const std::string& scope, const std::string& scope_id,
                                  const std::string& episode_order);

    std::unordered_map<std::string, CursorEntry>   cursors_;
    std::unordered_map<std::string, BlockPosition> block_positions_;
    std::unordered_map<std::string, int>           filler_positions_;
    std::unordered_map<std::string, SlotCursor>    slot_cursors_;
    std::unordered_map<std::string, std::deque<std::pair<std::string, std::time_t>>> recent_plays_;
    std::vector<PlayRecord>                         play_records_;
};
