#include "BlockRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>

BlockRepository::BlockRepository(Database& db) : db_(db) {}

std::vector<Block> BlockRepository::loadBlocks(const std::string& channel_id) {
    std::vector<Block> blocks;
    SQLite::Statement q(db_.get(), R"(
        SELECT block_id, block_type, day_mask, start_time, end_time,
               program_count, priority, max_content_rating, advancement, cursor_scope,
               late_start_mins, align_to_mins, inter_filler, early_start_secs,
               filler_selection, smart_pct, start_scope, no_history_behavior,
               max_consecutive_episodes,
               intro_content_type, intro_content_id,
               outro_content_type, outro_content_id,
               interstitial_content_type, interstitial_content_id, interstitial_every_n,
               snap_to_group_start, name
        FROM block WHERE channel_id = ?
        ORDER BY priority DESC
    )");
    q.bind(1, channel_id);

    while (q.executeStep()) {
        Block b;
        b.block_id           = q.getColumn(0).getString();
        b.channel_id         = channel_id;
        b.block_type         = parseBlockType(q.getColumn(1).getString());
        b.day_mask           = q.getColumn(2).getInt();
        b.start_time         = q.getColumn(3).getString();
        if (!q.getColumn(4).isNull()) b.end_time = q.getColumn(4).getString();
        b.program_count      = q.getColumn(5).getInt();
        b.priority           = q.getColumn(6).getInt();
        b.max_content_rating = q.getColumn(7).getString();
        b.advancement        = parseAdvancement(q.getColumn(8).getString());
        b.cursor_scope       = parseCursorScope(q.getColumn(9).getString());
        b.late_start_mins    = q.getColumn(10).getInt();
        b.align_to_mins      = q.getColumn(11).getInt();
        b.inter_filler       = q.getColumn(12).getInt() != 0;
        b.early_start_secs   = q.getColumn(13).getInt();
        b.filler_selection   = q.getColumn(14).getString();
        b.smart_pct                = q.getColumn(15).getInt();
        b.start_scope              = q.getColumn(16).getString();
        b.no_history_behavior      = parseNoHistoryBehavior(q.getColumn(17).getString());
        b.max_consecutive_episodes = q.getColumn(18).getInt();
        b.intro_content_type       = q.getColumn(19).getString();
        b.intro_content_id         = q.getColumn(20).getString();
        b.outro_content_type       = q.getColumn(21).getString();
        b.outro_content_id         = q.getColumn(22).getString();
        b.interstitial_content_type = q.getColumn(23).getString();
        b.interstitial_content_id   = q.getColumn(24).getString();
        b.interstitial_every_n      = q.getColumn(25).getInt();
        b.snap_to_group_start       = q.getColumn(26).getInt() != 0;
        // column 27: name (not stored in Block struct — kept for Router use)

        {
            SQLite::Statement cq(db_.get(), R"(
                SELECT id, content_type, content_id, position, season_filter, weight, run_count,
                       include_specials, episode_order
                FROM block_content WHERE block_id = ? ORDER BY position
            )");
            cq.bind(1, b.block_id);
            while (cq.executeStep()) {
                BlockContent bc;
                bc.id              = cq.getColumn(0).getInt();
                bc.block_id        = b.block_id;
                bc.content_type    = cq.getColumn(1).getString();
                bc.content_id      = cq.getColumn(2).getString();
                bc.position        = cq.getColumn(3).getInt();
                if (!cq.getColumn(4).isNull()) bc.season_filter = cq.getColumn(4).getInt();
                bc.weight           = cq.getColumn(5).getInt();
                bc.run_count        = cq.getColumn(6).getInt();
                bc.include_specials = cq.getColumn(7).getInt() != 0;
                bc.episode_order    = cq.getColumn(8).getString();
                b.content.push_back(std::move(bc));
            }
        }

        {
            SQLite::Statement fq(db_.get(), R"(
                SELECT content_type, content_id, advancement, weight, season_filter
                FROM block_filler_entry WHERE block_id = ? ORDER BY position
            )");
            fq.bind(1, b.block_id);
            while (fq.executeStep()) {
                BlockFillerEntry fe;
                fe.content_type = fq.getColumn(0).getString();
                fe.content_id   = fq.getColumn(1).getString();
                fe.advancement  = fq.getColumn(2).getString();
                fe.weight       = fq.getColumn(3).getInt();
                if (!fq.getColumn(4).isNull()) fe.season_filter = fq.getColumn(4).getInt();
                b.filler_entries.push_back(std::move(fe));
            }
        }

        blocks.push_back(std::move(b));
    }
    return blocks;
}

std::string BlockRepository::channelTimezone(const std::string& channel_id) {
    SQLite::Statement q(db_.get(), "SELECT timezone FROM channel WHERE channel_id=?");
    q.bind(1, channel_id);
    if (q.executeStep()) {
        auto tz = q.getColumn(0).getString();
        if (!tz.empty()) return tz;
    }
    return "UTC";
}

std::string BlockRepository::channelAdvanceMode(const std::string& channel_id) {
    SQLite::Statement q(db_.get(), "SELECT advance_mode FROM channel WHERE channel_id=?");
    q.bind(1, channel_id);
    if (q.executeStep()) {
        auto m = q.getColumn(0).getString();
        if (!m.empty()) return m;
    }
    return "scheduled";
}

int BlockRepository::readCursorPos(const std::string& content_type,
                                   const std::string& content_id,
                                   const std::string& scope,
                                   const std::string& scope_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT position FROM media_cursor
        WHERE content_type=? AND content_id=? AND cursor_scope=? AND scope_id=?
    )");
    q.bind(1, content_type); q.bind(2, content_id);
    q.bind(3, scope);        q.bind(4, scope_id);
    if (q.executeStep() && !q.getColumn(0).isNull())
        return q.getColumn(0).getInt();
    return 0;
}

void BlockRepository::writeCursorPos(const std::string& content_type,
                                     const std::string& content_id,
                                     const std::string& scope,
                                     const std::string& scope_id,
                                     int pos,
                                     const std::string& episode_id) {
    SQLite::Statement q(db_.get(), R"(
        INSERT INTO media_cursor
            (content_type, content_id, cursor_scope, scope_id, episode_id, position, updated_at)
        VALUES (?,?,?,?,?,?,?)
        ON CONFLICT(content_type, content_id, cursor_scope, scope_id)
        DO UPDATE SET position=excluded.position,
                      episode_id=excluded.episode_id,
                      updated_at=excluded.updated_at
    )");
    q.bind(1, content_type); q.bind(2, content_id);
    q.bind(3, scope);        q.bind(4, scope_id);
    if (episode_id.empty()) q.bind(5); else q.bind(5, episode_id);
    q.bind(6, pos);
    q.bind(7, static_cast<int64_t>(std::time(nullptr)));
    q.exec();
}

int BlockRepository::readBlockRR(const std::string& block_id,
                                  const std::string& channel_id) {
    SQLite::Statement q(db_.get(),
        "SELECT content_position FROM block_state WHERE block_id=? AND channel_id=?");
    q.bind(1, block_id); q.bind(2, channel_id);
    if (q.executeStep()) return q.getColumn(0).getInt();
    return 0;
}

void BlockRepository::writeBlockRR(const std::string& block_id,
                                    const std::string& channel_id, int pos) {
    SQLite::Statement q(db_.get(), R"(
        INSERT INTO block_state (block_id, channel_id, content_position, updated_at)
        VALUES (?,?,?,?)
        ON CONFLICT(block_id, channel_id)
        DO UPDATE SET content_position=excluded.content_position,
                      updated_at=excluded.updated_at
    )");
    q.bind(1, block_id); q.bind(2, channel_id);
    q.bind(3, pos);
    q.bind(4, static_cast<int64_t>(std::time(nullptr)));
    q.exec();
}

int BlockRepository::readRunsRemaining(const std::string& block_id,
                                        const std::string& channel_id) {
    SQLite::Statement q(db_.get(),
        "SELECT runs_remaining FROM block_state WHERE block_id=? AND channel_id=?");
    q.bind(1, block_id); q.bind(2, channel_id);
    if (q.executeStep()) return q.getColumn(0).getInt();
    return 0;
}

int BlockRepository::readConsecutiveCount(const std::string& block_id,
                                           const std::string& channel_id) {
    SQLite::Statement q(db_.get(),
        "SELECT consecutive_count FROM block_state WHERE block_id=? AND channel_id=?");
    q.bind(1, block_id); q.bind(2, channel_id);
    if (q.executeStep()) return q.getColumn(0).getInt();
    return 0;
}

void BlockRepository::writeRerunState(const std::string& block_id,
                                       const std::string& channel_id,
                                       int content_pos,
                                       int runs_remaining,
                                       int consecutive_count) {
    SQLite::Statement q(db_.get(), R"(
        INSERT INTO block_state (block_id, channel_id, content_position, runs_remaining,
                                 consecutive_count, updated_at)
        VALUES (?,?,?,?,?,?)
        ON CONFLICT(block_id, channel_id)
        DO UPDATE SET content_position=excluded.content_position,
                      runs_remaining=excluded.runs_remaining,
                      consecutive_count=excluded.consecutive_count,
                      updated_at=excluded.updated_at
    )");
    q.bind(1, block_id); q.bind(2, channel_id); q.bind(3, content_pos);
    q.bind(4, runs_remaining); q.bind(5, consecutive_count);
    q.bind(6, static_cast<int64_t>(std::time(nullptr)));
    q.exec();
}
