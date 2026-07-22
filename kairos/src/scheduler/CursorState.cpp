#include "CursorState.h"
#include "../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

namespace {
// Recency lists are bounded so a long-lived channel doesn't grow this
// unboundedly in the anchor blob — generous enough that no realistic
// smart_pct hot_count would ever need more than the most recent entries.
constexpr size_t kMaxRecentPlays = 300;
}

// ── Key helper ────────────────────────────────────────────────────────────────
// \x01 is used as separator; it cannot appear in content IDs or scope strings.

std::string CursorState::cursorKey(const std::string& ct, const std::string& cid,
                                    const std::string& scope, const std::string& scope_id,
                                    const std::string& episode_order) {
    std::string k;
    k.reserve(ct.size() + cid.size() + scope.size() + scope_id.size() + episode_order.size() + 5);
    k += ct;             k += '\x01';
    k += cid;            k += '\x01';
    k += scope;          k += '\x01';
    k += scope_id;       k += '\x01';
    k += episode_order;
    return k;
}

// ── Media cursor accessors ────────────────────────────────────────────────────

int CursorState::getCursorPos(const std::string& ct, const std::string& cid,
                               const std::string& scope, const std::string& scope_id,
                               const std::string& episode_order) const {
    auto it = cursors_.find(cursorKey(ct, cid, scope, scope_id, episode_order));
    return it != cursors_.end() ? it->second.position : 0;
}

std::string CursorState::getCursorEpisodeId(const std::string& ct, const std::string& cid,
                                              const std::string& scope, const std::string& scope_id,
                                              const std::string& episode_order) const {
    auto it = cursors_.find(cursorKey(ct, cid, scope, scope_id, episode_order));
    return it != cursors_.end() ? it->second.episode_id : "";
}

std::vector<std::string> CursorState::getCursorAhead(const std::string& ct, const std::string& cid,
                                                       const std::string& scope, const std::string& scope_id,
                                                       const std::string& episode_order) const {
    auto it = cursors_.find(cursorKey(ct, cid, scope, scope_id, episode_order));
    return it != cursors_.end() ? it->second.ahead : std::vector<std::string>{};
}

void CursorState::setCursorPos(const std::string& ct, const std::string& cid,
                                const std::string& scope, const std::string& scope_id,
                                int pos, const std::string& episode_id,
                                const std::string& episode_order,
                                const std::vector<std::string>& ahead) {
    auto& entry = cursors_[cursorKey(ct, cid, scope, scope_id, episode_order)];
    entry.content_type  = ct;
    entry.content_id    = cid;
    entry.cursor_scope  = scope;
    entry.scope_id      = scope_id;
    entry.episode_order = episode_order;
    entry.position       = pos;
    entry.episode_id      = episode_id;
    entry.ahead          = ahead;
}

bool CursorState::hasCursor(const std::string& ct, const std::string& cid,
                            const std::string& scope, const std::string& scope_id,
                            const std::string& episode_order) const {
    return cursors_.contains(cursorKey(ct, cid, scope, scope_id, episode_order));
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

void CursorState::setSlotCursor(const std::string& slot_id, int queue_pos,
                                 const std::string& episode_watermark_id,
                                 const std::vector<std::string>& episode_ahead) {
    slot_cursors_[slot_id] = {queue_pos, episode_watermark_id, episode_ahead};
}

bool CursorState::hasSlotCursor(const std::string& slot_id) const {
    return slot_cursors_.contains(slot_id);
}

// ── Recency (Smart-cooldown) ─────────────────────────────────────────────────

void CursorState::recordRecentPlay(const std::string& domain_key, const std::string& item_id,
                                    std::time_t at) {
    auto& dq = recent_plays_[domain_key];
    dq.emplace_front(item_id, at);
    while (dq.size() > kMaxRecentPlays) dq.pop_back();
}

std::vector<std::pair<std::string, std::time_t>> CursorState::recentPlays(const std::string& domain_key) const {
    auto it = recent_plays_.find(domain_key);
    if (it == recent_plays_.end()) return {};
    return std::vector<std::pair<std::string, std::time_t>>(it->second.begin(), it->second.end());
}

// ── Play history ──────────────────────────────────────────────────────────────

void CursorState::addPlayRecord(const std::string& channel_id, const std::string& item_type,
                                 const std::string& item_id, const std::string& show_id,
                                 const std::string& block_id, std::time_t aired_at) {
    play_records_.push_back({channel_id, item_type, item_id, show_id, block_id, aired_at});
}

// ── DB I/O ────────────────────────────────────────────────────────────────────

CursorState CursorState::loadFromDB(Database& db, const std::string& channel_id) {
    CursorState state;

    // Channel-scoped and block-scoped cursors.
    {
        SQLite::Statement q(db.get(), R"(
            SELECT content_type, content_id, cursor_scope, scope_id, episode_order,
                   position, COALESCE(episode_id, ''), ahead
            FROM media_cursor
            WHERE (cursor_scope = 'channel' AND scope_id = ?)
               OR (cursor_scope = 'block'
                   AND scope_id IN (SELECT block_id FROM block WHERE channel_id = ?))
        )");
        q.bind(1, channel_id);
        q.bind(2, channel_id);
        while (q.executeStep()) {
            std::vector<std::string> ahead;
            try {
                auto aj = json::parse(q.getColumn(7).getString());
                if (aj.is_array()) for (auto& v : aj) ahead.push_back(v.get<std::string>());
            } catch (...) {}
            state.setCursorPos(
                q.getColumn(0).getString(),
                q.getColumn(1).getString(),
                q.getColumn(2).getString(),
                q.getColumn(3).getString(),
                q.getColumn(5).getInt(),
                q.getColumn(6).getString(),
                q.getColumn(4).getString(),
                ahead
            );
        }
    }

    // Global-scoped cursors referenced by this channel's blocks.
    {
        SQLite::Statement q(db.get(), R"(
            SELECT content_type, content_id, cursor_scope, scope_id, episode_order,
                   position, COALESCE(episode_id, ''), ahead
            FROM media_cursor
            WHERE cursor_scope = 'global'
              AND content_id IN (
                  SELECT DISTINCT content_id FROM block_content
                  WHERE block_id IN (SELECT block_id FROM block WHERE channel_id = ?)
              )
        )");
        q.bind(1, channel_id);
        while (q.executeStep()) {
            std::vector<std::string> ahead;
            try {
                auto aj = json::parse(q.getColumn(7).getString());
                if (aj.is_array()) for (auto& v : aj) ahead.push_back(v.get<std::string>());
            } catch (...) {}
            state.setCursorPos(
                q.getColumn(0).getString(),
                q.getColumn(1).getString(),
                q.getColumn(2).getString(),
                q.getColumn(3).getString(),
                q.getColumn(5).getInt(),
                q.getColumn(6).getString(),
                q.getColumn(4).getString(),
                ahead
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
            SELECT ts.slot_id, ts.queue_pos, COALESCE(ts.episode_watermark_id, ''), ts.episode_ahead
            FROM timeslot_slot ts
            JOIN block b ON ts.block_id = b.block_id
            WHERE b.channel_id = ?
        )");
        q.bind(1, channel_id);
        while (q.executeStep()) {
            std::vector<std::string> ahead;
            try {
                auto aj = json::parse(q.getColumn(3).getString());
                if (aj.is_array()) for (auto& v : aj) ahead.push_back(v.get<std::string>());
            } catch (...) {}
            state.setSlotCursor(q.getColumn(0).getString(),
                                q.getColumn(1).getInt(),
                                q.getColumn(2).getString(),
                                ahead);
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
            (content_type, content_id, cursor_scope, scope_id, episode_order,
             position, episode_id, ahead, updated_at)
        VALUES (?,?,?,?,?,?,?,?,strftime('%s','now'))
    )");

    for (const auto& [key, entry] : cursors_) {
        ins.bind(1, entry.content_type);
        ins.bind(2, entry.content_id);
        ins.bind(3, entry.cursor_scope);
        ins.bind(4, entry.scope_id);
        ins.bind(5, entry.episode_order);
        ins.bind(6, entry.position);
        if (entry.episode_id.empty()) ins.bind(7); else ins.bind(7, entry.episode_id);
        ins.bind(8, json(entry.ahead).dump());
        try {
            ins.exec();
        } catch (const std::exception& e) {
            // entry.episode_id is a stale watermark — it pointed at an episode
            // that's since been deleted (orphan cleanup nulls *live*
            // media_cursor rows when that happens, see SyncManager::
            // cleanupOrphans, but never scrubs a channel's anchor_hashes
            // snapshot or block_content itself, so a stale id can resurface
            // here months later once deserialized back out of an anchor blob).
            // Same "skip just this one, don't abort the whole channel's EPG"
            // reasoning as commit()'s scheduled_program insert loop just
            // above this call in EPGMaterializer.cpp — this INSERT sits
            // inside that same sp_commit savepoint, so an uncaught throw
            // here previously rolled back the *entire* commit (including
            // every already-inserted scheduled_program row) and 500'd
            // GET /api/channels/:id/epg for the whole channel over one
            // dangling reference. Logged so a missing-media gap is visible
            // (surfaced in Hades' log viewer) rather than silently dropped.
            std::cerr << "[epg] WARNING: dropping stale cursor content=" << entry.content_type << ":" << entry.content_id
                      << " scope=" << entry.cursor_scope << ":" << entry.scope_id
                      << " episode_id=" << entry.episode_id << " (no longer exists): " << e.what() << '\n';
        }
        // tryReset(), not reset() — after a failed exec(), sqlite3_reset()
        // returns that same error code again (SQLite re-surfaces the last
        // step's failure), and Statement::reset() throws on a non-OK code —
        // which would silently defeat the catch above by re-throwing right
        // past it. tryReset() is the noexcept variant for exactly this.
        ins.tryReset();
    }

    // Block state: delete-then-reinsert.
    {
        SQLite::Statement d(db.get(), "DELETE FROM block_state WHERE channel_id=?");
        d.bind(1, channel_id); d.exec();
    }

    SQLite::Statement bsins(db.get(), R"(
        INSERT OR IGNORE INTO block_state (block_id, channel_id, content_position,
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
            "UPDATE timeslot_slot SET queue_pos=?, episode_watermark_id=?, episode_ahead=? WHERE slot_id=?");
        for (const auto& [slot_id, sc] : slot_cursors_) {
            su.bind(1, sc.queue_pos);
            if (sc.episode_watermark_id.empty()) su.bind(2); else su.bind(2, sc.episode_watermark_id);
            su.bind(3, json(sc.episode_ahead).dump());
            su.bind(4, slot_id);
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

    SQLite::Statement d4(db.get(), R"(
        UPDATE timeslot_slot SET queue_pos=0, episode_watermark_id=NULL, episode_ahead='[]'
        WHERE block_id IN (SELECT block_id FROM block WHERE channel_id=?)
    )");
    d4.bind(1, channel_id); d4.exec();
}

// ── Anchor serialization ──────────────────────────────────────────────────────

std::string CursorState::serializeCursors() const {
    json cursors_arr = json::array();
    for (const auto& [key, entry] : cursors_) {
        cursors_arr.push_back({
            {"content_type",  entry.content_type},
            {"content_id",    entry.content_id},
            {"cursor_scope",  entry.cursor_scope},
            {"scope_id",      entry.scope_id},
            {"episode_order", entry.episode_order},
            {"position",      entry.position},
            {"episode_id",    entry.episode_id},
            {"ahead",         entry.ahead}
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

    json slot_cursors_arr = json::array();
    for (const auto& [slot_id, sc] : slot_cursors_) {
        slot_cursors_arr.push_back({
            {"slot_id",               slot_id},
            {"queue_pos",             sc.queue_pos},
            {"episode_watermark_id",  sc.episode_watermark_id},
            {"episode_ahead",         sc.episode_ahead}
        });
    }

    json filler_positions_arr = json::array();
    for (const auto& [key, pos] : filler_positions_) {
        filler_positions_arr.push_back({{"key", key}, {"position", pos}});
    }

    // Oldest-first per domain: recent_plays_ stores newest-at-front (deque
    // front = most recent), but deserializeCursors() replays entries through
    // recordRecentPlay()'s push_front — feeding them oldest-first reconstructs
    // the original newest-first ordering instead of reversing it.
    json recent_plays_arr = json::array();
    for (const auto& [domain_key, dq] : recent_plays_) {
        for (auto it = dq.rbegin(); it != dq.rend(); ++it) {
            recent_plays_arr.push_back({
                {"domain_key", domain_key},
                {"item_id",    it->first},
                {"at",         static_cast<int64_t>(it->second)}
            });
        }
    }

    return json{
        {"cursors",          cursors_arr},
        {"block_states",     block_states_arr},
        {"slot_cursors",     slot_cursors_arr},
        {"filler_positions", filler_positions_arr},
        {"recent_plays",     recent_plays_arr}
    }.dump();
}

CursorState CursorState::deserializeCursors(const std::string& json_str) {
    CursorState state;
    try {
        auto j = json::parse(json_str);

        if (j.contains("cursors")) {
            for (const auto& c : j["cursors"]) {
                std::vector<std::string> ahead;
                if (c.contains("ahead") && c["ahead"].is_array())
                    for (auto& v : c["ahead"]) ahead.push_back(v.get<std::string>());
                state.setCursorPos(
                    c.value("content_type", ""),
                    c.value("content_id",   ""),
                    c.value("cursor_scope", "block"),
                    c.value("scope_id",     ""),
                    c.value("position",     0),
                    c.value("episode_id",   ""),
                    c.value("episode_order", ""),
                    ahead
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

        if (j.contains("slot_cursors")) {
            for (const auto& sc : j["slot_cursors"]) {
                std::vector<std::string> ahead;
                if (sc.contains("episode_ahead") && sc["episode_ahead"].is_array())
                    for (auto& v : sc["episode_ahead"]) ahead.push_back(v.get<std::string>());
                state.setSlotCursor(
                    sc.value("slot_id", ""),
                    sc.value("queue_pos", 0),
                    sc.value("episode_watermark_id", ""),
                    ahead
                );
            }
        }

        if (j.contains("filler_positions")) {
            for (const auto& fp : j["filler_positions"]) {
                state.fillerPos(fp.value("key", "")) = fp.value("position", 0);
            }
        }

        if (j.contains("recent_plays")) {
            // Stored most-recent-first (append order from serializeCursors' deque
            // iteration); replay in the same order so recordRecentPlay's
            // push_front reconstructs the original ordering.
            for (const auto& rp : j["recent_plays"]) {
                state.recordRecentPlay(
                    rp.value("domain_key", ""),
                    rp.value("item_id",    ""),
                    static_cast<std::time_t>(rp.value("at", (int64_t)0))
                );
            }
        }
    } catch (...) {}
    return state;
}
