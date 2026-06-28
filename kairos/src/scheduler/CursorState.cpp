#include "CursorState.h"
#include "../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

// ── Key helper ────────────────────────────────────────────────────────────────
// \x01 is used as separator; it cannot appear in content IDs or scope strings.

std::string CursorState::cursorKey(const std::string& ct, const std::string& cid,
                                    const std::string& scope, const std::string& scope_id) {
    std::string k;
    k.reserve(ct.size() + cid.size() + scope.size() + scope_id.size() + 4);
    k += ct;     k += '\x01';
    k += cid;    k += '\x01';
    k += scope;  k += '\x01';
    k += scope_id;
    return k;
}

// ── Media cursor accessors ────────────────────────────────────────────────────

int CursorState::getCursorPos(const std::string& ct, const std::string& cid,
                               const std::string& scope, const std::string& scope_id) const {
    auto it = cursors_.find(cursorKey(ct, cid, scope, scope_id));
    return it != cursors_.end() ? it->second.position : 0;
}

void CursorState::setCursorPos(const std::string& ct, const std::string& cid,
                                const std::string& scope, const std::string& scope_id,
                                int pos, const std::string& episode_id) {
    auto& entry = cursors_[cursorKey(ct, cid, scope, scope_id)];
    entry.content_type  = ct;
    entry.content_id    = cid;
    entry.cursor_scope  = scope;
    entry.scope_id      = scope_id;
    entry.position      = pos;
    entry.episode_id    = episode_id;
}

// ── Block state accessors ─────────────────────────────────────────────────────

int CursorState::getContentPosition(const std::string& block_id) const {
    auto it = block_positions_.find(block_id);
    return it != block_positions_.end() ? it->second.content_position : 0;
}

int CursorState::getRunsRemaining(const std::string& block_id) const {
    auto it = block_positions_.find(block_id);
    return it != block_positions_.end() ? it->second.runs_remaining : 0;
}

int CursorState::getConsecutiveCount(const std::string& block_id) const {
    auto it = block_positions_.find(block_id);
    return it != block_positions_.end() ? it->second.consecutive_count : 0;
}

bool CursorState::hasBlockPosition(const std::string& block_id) const {
    return block_positions_.contains(block_id);
}

void CursorState::setBlockPosition(const std::string& block_id,
                                    int content_position, int runs_remaining,
                                    int consecutive_count) {
    block_positions_[block_id] = {content_position, runs_remaining, consecutive_count};
}

void CursorState::setContentPosition(const std::string& block_id, int content_position) {
    auto& bp = block_positions_[block_id];
    bp.content_position = content_position;
}

// ── Filler position accessors ─────────────────────────────────────────────────

int& CursorState::fillerPos(const std::string& key) {
    return filler_positions_[key];
}

int CursorState::getFillerPos(const std::string& key) const {
    auto it = filler_positions_.find(key);
    return it != filler_positions_.end() ? it->second : 0;
}

bool CursorState::hasFillerPos(const std::string& key) const {
    return filler_positions_.contains(key);
}

// ── Timeslot slot cursor accessors ───────────────────────────────────────────

SlotCursor CursorState::getSlotCursor(const std::string& slot_id) const {
    auto it = slot_cursors_.find(slot_id);
    return it != slot_cursors_.end() ? it->second : SlotCursor{};
}

void CursorState::setSlotCursor(const std::string& slot_id, int queue_pos, int episode_pos) {
    slot_cursors_[slot_id] = {queue_pos, episode_pos};
}

bool CursorState::hasSlotCursor(const std::string& slot_id) const {
    return slot_cursors_.contains(slot_id);
}

// ── Play history ──────────────────────────────────────────────────────────────

void CursorState::addPlayRecord(const std::string& channel_id, const std::string& item_type,
                                 const std::string& item_id, const std::string& block_id,
                                 std::time_t aired_at) {
    play_records_.push_back({channel_id, item_type, item_id, block_id, aired_at});
}

// ── DB I/O ────────────────────────────────────────────────────────────────────

CursorState CursorState::loadFromDB(Database& db, const std::string& channel_id) {
    CursorState state;

    // Channel-scoped and block-scoped cursors.
    {
        SQLite::Statement q(db.get(), R"(
            SELECT content_type, content_id, cursor_scope, scope_id,
                   position, COALESCE(episode_id, '')
            FROM media_cursor
            WHERE (cursor_scope = 'channel' AND scope_id = ?)
               OR (cursor_scope = 'block'
                   AND scope_id IN (SELECT block_id FROM block WHERE channel_id = ?))
        )");
        q.bind(1, channel_id);
        q.bind(2, channel_id);
        while (q.executeStep()) {
            state.setCursorPos(
                q.getColumn(0).getString(),
                q.getColumn(1).getString(),
                q.getColumn(2).getString(),
                q.getColumn(3).getString(),
                q.getColumn(4).getInt(),
                q.getColumn(5).getString()
            );
        }
    }

    // Global-scoped cursors referenced by this channel's blocks.
    {
        SQLite::Statement q(db.get(), R"(
            SELECT content_type, content_id, cursor_scope, scope_id,
                   position, COALESCE(episode_id, '')
            FROM media_cursor
            WHERE cursor_scope = 'global'
              AND content_id IN (
                  SELECT DISTINCT content_id FROM block_content
                  WHERE block_id IN (SELECT block_id FROM block WHERE channel_id = ?)
              )
        )");
        q.bind(1, channel_id);
        while (q.executeStep()) {
            state.setCursorPos(
                q.getColumn(0).getString(),
                q.getColumn(1).getString(),
                q.getColumn(2).getString(),
                q.getColumn(3).getString(),
                q.getColumn(4).getInt(),
                q.getColumn(5).getString()
            );
        }
    }

    // Block state.
    {
        SQLite::Statement q(db.get(), R"(
            SELECT block_id, content_position, runs_remaining, consecutive_count
            FROM block_state WHERE channel_id = ?
        )");
        q.bind(1, channel_id);
        while (q.executeStep()) {
            state.setBlockPosition(
                q.getColumn(0).getString(),
                q.getColumn(1).getInt(),
                q.getColumn(2).getInt(),
                q.getColumn(3).getInt()
            );
        }
    }

    // Timeslot slot cursors.
    {
        SQLite::Statement q(db.get(), R"(
            SELECT ts.slot_id, ts.queue_pos, ts.episode_pos
            FROM timeslot_slot ts
            JOIN block b ON ts.block_id = b.block_id
            WHERE b.channel_id = ?
        )");
        q.bind(1, channel_id);
        while (q.executeStep()) {
            state.setSlotCursor(q.getColumn(0).getString(),
                                q.getColumn(1).getInt(),
                                q.getColumn(2).getInt());
        }
    }

    return state;
}

void CursorState::applyToDB(Database& db, const std::string& channel_id) const {
    // Channel-scoped and block-scoped: delete-then-reinsert for correctness
    // (handles cursors that were removed as well as new/updated ones).
    {
        SQLite::Statement d1(db.get(),
            "DELETE FROM media_cursor WHERE cursor_scope='channel' AND scope_id=?");
        d1.bind(1, channel_id); d1.exec();

        SQLite::Statement d2(db.get(), R"(
            DELETE FROM media_cursor WHERE cursor_scope='block'
            AND scope_id IN (SELECT block_id FROM block WHERE channel_id=?)
        )");
        d2.bind(1, channel_id); d2.exec();
    }

    SQLite::Statement ins(db.get(), R"(
        INSERT OR REPLACE INTO media_cursor
            (content_type, content_id, cursor_scope, scope_id, position, episode_id, updated_at)
        VALUES (?,?,?,?,?,?,strftime('%s','now'))
    )");

    for (const auto& [key, entry] : cursors_) {
        ins.bind(1, entry.content_type);
        ins.bind(2, entry.content_id);
        ins.bind(3, entry.cursor_scope);
        ins.bind(4, entry.scope_id);
        ins.bind(5, entry.position);
        if (entry.episode_id.empty()) ins.bind(6); else ins.bind(6, entry.episode_id);
        ins.exec();
        ins.reset();
    }

    // Block state: delete-then-reinsert.
    {
        SQLite::Statement d(db.get(), "DELETE FROM block_state WHERE channel_id=?");
        d.bind(1, channel_id); d.exec();
    }

    SQLite::Statement bsins(db.get(), R"(
        INSERT INTO block_state (block_id, channel_id, content_position,
                                 runs_remaining, consecutive_count, updated_at)
        VALUES (?,?,?,?,?,strftime('%s','now'))
    )");
    for (const auto& [block_id, bp] : block_positions_) {
        bsins.bind(1, block_id);
        bsins.bind(2, channel_id);
        bsins.bind(3, bp.content_position);
        bsins.bind(4, bp.runs_remaining);
        bsins.bind(5, bp.consecutive_count);
        bsins.exec();
        bsins.reset();
    }

    // Timeslot slot cursors.
    if (!slot_cursors_.empty()) {
        SQLite::Statement su(db.get(),
            "UPDATE timeslot_slot SET queue_pos=?, episode_pos=? WHERE slot_id=?");
        for (const auto& [slot_id, sc] : slot_cursors_) {
            su.bind(1, sc.queue_pos);
            su.bind(2, sc.episode_pos);
            su.bind(3, slot_id);
            su.exec();
            su.reset();
        }
    }
}

void CursorState::clearFromDB(Database& db, const std::string& channel_id) {
    SQLite::Statement d1(db.get(),
        "DELETE FROM media_cursor WHERE cursor_scope='channel' AND scope_id=?");
    d1.bind(1, channel_id); d1.exec();

    SQLite::Statement d2(db.get(), R"(
        DELETE FROM media_cursor WHERE cursor_scope='block'
        AND scope_id IN (SELECT block_id FROM block WHERE channel_id=?)
    )");
    d2.bind(1, channel_id); d2.exec();

    SQLite::Statement d3(db.get(), "DELETE FROM block_state WHERE channel_id=?");
    d3.bind(1, channel_id); d3.exec();
}

// ── Anchor serialization ──────────────────────────────────────────────────────

std::string CursorState::serializeCursors() const {
    json cursors_arr = json::array();
    for (const auto& [key, entry] : cursors_) {
        cursors_arr.push_back({
            {"content_type", entry.content_type},
            {"content_id",   entry.content_id},
            {"cursor_scope", entry.cursor_scope},
            {"scope_id",     entry.scope_id},
            {"position",     entry.position},
            {"episode_id",   entry.episode_id}
        });
    }

    json block_states_arr = json::array();
    for (const auto& [block_id, bp] : block_positions_) {
        block_states_arr.push_back({
            {"block_id",          block_id},
            {"content_position",  bp.content_position},
            {"runs_remaining",    bp.runs_remaining},
            {"consecutive_count", bp.consecutive_count}
        });
    }

    return json{{"cursors", cursors_arr}, {"block_states", block_states_arr}}.dump();
}

CursorState CursorState::deserializeCursors(const std::string& json_str) {
    CursorState state;
    try {
        auto j = json::parse(json_str);

        if (j.contains("cursors")) {
            for (const auto& c : j["cursors"]) {
                state.setCursorPos(
                    c.value("content_type", ""),
                    c.value("content_id",   ""),
                    c.value("cursor_scope", "block"),
                    c.value("scope_id",     ""),
                    c.value("position",     0),
                    c.value("episode_id",   "")
                );
            }
        }

        if (j.contains("block_states")) {
            for (const auto& bs : j["block_states"]) {
                state.setBlockPosition(
                    bs.value("block_id",          ""),
                    bs.value("content_position",  0),
                    bs.value("runs_remaining",     0),
                    bs.value("consecutive_count", 0)
                );
            }
        }
    } catch (...) {}
    return state;
}
