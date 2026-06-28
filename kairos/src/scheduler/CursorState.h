#pragma once
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

class Database;

// Cursor for one timeslot slot: which queue item and episode-within-item we're on.
struct SlotCursor {
    int queue_pos   = 0;
    int episode_pos = 0;
};

// A single media cursor entry.
struct CursorEntry {
    std::string content_type;
    std::string content_id;
    std::string cursor_scope; // "global" | "channel" | "block"
    std::string scope_id;     // "" for global, channel_id, or block_id
    int         position   = 0;
    std::string episode_id;
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
    int  getCursorPos(const std::string& content_type, const std::string& content_id,
                      const std::string& scope, const std::string& scope_id) const;
    void setCursorPos(const std::string& content_type, const std::string& content_id,
                      const std::string& scope, const std::string& scope_id,
                      int pos, const std::string& episode_id = "");

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
    void        setSlotCursor(const std::string& slot_id, int queue_pos, int episode_pos);
    bool        hasSlotCursor(const std::string& slot_id) const;
    const std::unordered_map<std::string, SlotCursor>& slotCursors() const { return slot_cursors_; }

    // ── Filler positions (absorbs SimState::show_pos) ─────────────────────────
    // Keys follow the existing SimState conventions:
    //   "fl_rr:<block_id>"                          — filler list round-robin position
    //   "fl_pos:<content_type>:<content_id>:<block_id>" — item position within a list
    int& fillerPos(const std::string& key);
    int  getFillerPos(const std::string& key) const;
    bool hasFillerPos(const std::string& key) const;

    // ── In-pass play records ──────────────────────────────────────────────────
    // Accumulates items scheduled during this projection pass.
    // Written to play_history (is_scheduled=1) by EPGMaterializer::commit(), not during project().
    void addPlayRecord(const std::string& channel_id, const std::string& item_type,
                       const std::string& item_id, const std::string& block_id,
                       std::time_t aired_at);
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
    // Produces the {"cursors": [...], "block_states": [...]} object embedded in
    // anchor_hashes. Field names match the existing DB schema / JSON format exactly
    // so stored anchors remain compatible.
    std::string serializeCursors() const;
    static CursorState deserializeCursors(const std::string& json_str);

private:
    static std::string cursorKey(const std::string& content_type, const std::string& content_id,
                                  const std::string& scope, const std::string& scope_id);

    std::unordered_map<std::string, CursorEntry>   cursors_;
    std::unordered_map<std::string, BlockPosition> block_positions_;
    std::unordered_map<std::string, int>           filler_positions_;
    std::unordered_map<std::string, SlotCursor>    slot_cursors_;
    std::vector<PlayRecord>                         play_records_;
};
