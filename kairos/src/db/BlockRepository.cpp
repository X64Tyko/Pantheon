#include "BlockRepository.h"
#include "Database.h"
#include "DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>
#include <stdexcept>

BlockRepository::BlockRepository(Database& db) : db_(db) {}

std::vector<Block> BlockRepository::loadBlocks(const std::string& channel_id) {
    std::vector<Block> blocks;
    SQLite::Statement q(db_.get(), R"(
        SELECT block_id, block_type, day_mask, start_time, end_time,
               program_count, priority, play_style, advancement, cursor_scope,
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
        b.play_style         = parsePlayStyle(q.getColumn(7).getString());
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

        if (b.block_type == BlockType::Timeslot) {
            SQLite::Statement sq(db_.get(), R"(
                SELECT slot_id, slot_index, slot_offset_mins, slot_duration_mins,
                       overflow, late_start_mins, early_start_secs, align_to_mins,
                       start_scope, queue_pos, episode_pos
                FROM timeslot_slot WHERE block_id = ? ORDER BY slot_index
            )");
            sq.bind(1, b.block_id);
            while (sq.executeStep()) {
                TimeslotSlot slot;
                slot.slot_id           = sq.getColumn(0).getString();
                slot.block_id          = b.block_id;
                slot.slot_index        = sq.getColumn(1).getInt();
                slot.slot_offset_mins  = sq.getColumn(2).getInt();
                slot.slot_duration_mins = sq.getColumn(3).getInt();
                slot.overflow          = parseSlotOverflow(sq.getColumn(4).getString());
                slot.late_start_mins   = sq.getColumn(5).getInt();
                slot.early_start_secs  = sq.getColumn(6).getInt();
                slot.align_to_mins     = sq.getColumn(7).getInt();
                slot.start_scope       = sq.getColumn(8).getString();
                slot.queue_pos         = sq.getColumn(9).getInt();
                slot.episode_pos       = sq.getColumn(10).getInt();
                SQLite::Statement qq(db_.get(), R"(
                    SELECT entry_id, queue_index, content_type, content_id,
                           COALESCE(premiere_date,''), pre_premiere_behavior
                    FROM timeslot_slot_queue WHERE slot_id = ? ORDER BY queue_index
                )");
                qq.bind(1, slot.slot_id);
                while (qq.executeStep()) {
                    TimeslotQueueEntry e;
                    e.entry_id              = qq.getColumn(0).getString();
                    e.queue_index           = qq.getColumn(1).getInt();
                    e.content_type          = qq.getColumn(2).getString();
                    e.content_id            = qq.getColumn(3).getString();
                    e.premiere_date         = qq.getColumn(4).getString();
                    e.pre_premiere_behavior = qq.getColumn(5).getString();
                    slot.queue.push_back(std::move(e));
                }
                b.slots.push_back(std::move(slot));
            }
        }

        blocks.push_back(std::move(b));
    }
    return blocks;
}

std::optional<Block> BlockRepository::loadBlock(const std::string& block_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT block_id, block_type, day_mask, start_time, end_time,
               program_count, priority, play_style, advancement, cursor_scope,
               late_start_mins, align_to_mins, inter_filler, early_start_secs,
               filler_selection, smart_pct, start_scope, no_history_behavior,
               max_consecutive_episodes,
               intro_content_type, intro_content_id,
               outro_content_type, outro_content_id,
               interstitial_content_type, interstitial_content_id, interstitial_every_n,
               snap_to_group_start, channel_id
        FROM block WHERE block_id = ?
    )");
    q.bind(1, block_id);
    if (!q.executeStep()) return std::nullopt;

    Block b;
    b.block_id           = q.getColumn(0).getString();
    b.block_type         = parseBlockType(q.getColumn(1).getString());
    b.day_mask           = q.getColumn(2).getInt();
    b.start_time         = q.getColumn(3).getString();
    if (!q.getColumn(4).isNull()) b.end_time = q.getColumn(4).getString();
    b.program_count      = q.getColumn(5).getInt();
    b.priority           = q.getColumn(6).getInt();
    b.play_style         = parsePlayStyle(q.getColumn(7).getString());
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
    b.channel_id                = q.getColumn(27).getString();

    {
        SQLite::Statement cq(db_.get(), R"(
            SELECT id, content_type, content_id, position, season_filter, weight, run_count,
                   include_specials, episode_order
            FROM block_content WHERE block_id = ? ORDER BY position
        )");
        cq.bind(1, block_id);
        while (cq.executeStep()) {
            BlockContent bc;
            bc.id              = cq.getColumn(0).getInt();
            bc.block_id        = block_id;
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
        fq.bind(1, block_id);
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

    if (b.block_type == BlockType::Timeslot) {
        SQLite::Statement sq(db_.get(), R"(
            SELECT slot_id, slot_index, slot_offset_mins, slot_duration_mins,
                   overflow, late_start_mins, early_start_secs, align_to_mins,
                   start_scope, queue_pos, episode_pos
            FROM timeslot_slot WHERE block_id = ? ORDER BY slot_index
        )");
        sq.bind(1, block_id);
        while (sq.executeStep()) {
            TimeslotSlot slot;
            slot.slot_id            = sq.getColumn(0).getString();
            slot.block_id           = block_id;
            slot.slot_index         = sq.getColumn(1).getInt();
            slot.slot_offset_mins   = sq.getColumn(2).getInt();
            slot.slot_duration_mins = sq.getColumn(3).getInt();
            slot.overflow           = parseSlotOverflow(sq.getColumn(4).getString());
            slot.late_start_mins    = sq.getColumn(5).getInt();
            slot.early_start_secs   = sq.getColumn(6).getInt();
            slot.align_to_mins      = sq.getColumn(7).getInt();
            slot.start_scope        = sq.getColumn(8).getString();
            slot.queue_pos          = sq.getColumn(9).getInt();
            slot.episode_pos        = sq.getColumn(10).getInt();
            SQLite::Statement qq(db_.get(), R"(
                SELECT entry_id, queue_index, content_type, content_id,
                       COALESCE(premiere_date,''), pre_premiere_behavior
                FROM timeslot_slot_queue WHERE slot_id = ? ORDER BY queue_index
            )");
            qq.bind(1, slot.slot_id);
            while (qq.executeStep()) {
                TimeslotQueueEntry e;
                e.entry_id              = qq.getColumn(0).getString();
                e.queue_index           = qq.getColumn(1).getInt();
                e.content_type          = qq.getColumn(2).getString();
                e.content_id            = qq.getColumn(3).getString();
                e.premiere_date         = qq.getColumn(4).getString();
                e.pre_premiere_behavior = qq.getColumn(5).getString();
                slot.queue.push_back(std::move(e));
            }
            b.slots.push_back(std::move(slot));
        }
    }

    return b;
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

// ---------------------------------------------------------------------------
// Block list with content (for API)
// ---------------------------------------------------------------------------

nlohmann::json BlockRepository::listWithContent(const std::string& channel_id) {
	SQLite::Statement q(db_.get(), R"(
		SELECT block_id, block_type, day_mask, start_time, end_time,
		       program_count, priority, play_style, advancement, cursor_scope,
		       late_start_mins, align_to_mins, inter_filler, early_start_secs,
		       filler_selection, smart_pct, start_scope, no_history_behavior,
		       max_consecutive_episodes, name,
		       intro_content_type, intro_content_id,
		       outro_content_type, outro_content_id,
		       interstitial_content_type, interstitial_content_id,
		       interstitial_every_n, snap_to_group_start
		FROM block WHERE channel_id = ?
		ORDER BY start_time, priority DESC
	)");
	q.bind(1, channel_id);

	nlohmann::json result = nlohmann::json::array();
	while (q.executeStep()) {
		std::string bid = q.getColumn(0).getString();
		nlohmann::json block = {
			{"block_id",                   bid},
			{"channel_id",                 channel_id},
			{"block_type",                 q.getColumn(1).getString()},
			{"day_mask",                   q.getColumn(2).getInt()},
			{"start_time",                 q.getColumn(3).getString()},
			{"program_count",              q.getColumn(5).getInt()},
			{"priority",                   q.getColumn(6).getInt()},
			{"play_style",                 q.getColumn(7).getString()},
			{"advancement",                q.getColumn(8).getString()},
			{"cursor_scope",               q.getColumn(9).getString()},
			{"late_start_mins",            q.getColumn(10).getInt()},
			{"align_to_mins",              q.getColumn(11).getInt()},
			{"inter_filler",               q.getColumn(12).getInt() != 0},
			{"early_start_secs",           q.getColumn(13).getInt()},
			{"filler_selection",           q.getColumn(14).getString()},
			{"smart_pct",                  q.getColumn(15).getInt()},
			{"start_scope",                q.getColumn(16).getString()},
			{"no_history_behavior",        q.getColumn(17).getString()},
			{"max_consecutive_episodes",   q.getColumn(18).getInt()},
			{"name",                       q.getColumn(19).getString()},
			{"intro_content_type",         q.getColumn(20).getString()},
			{"intro_content_id",           q.getColumn(21).getString()},
			{"outro_content_type",         q.getColumn(22).getString()},
			{"outro_content_id",           q.getColumn(23).getString()},
			{"interstitial_content_type",  q.getColumn(24).getString()},
			{"interstitial_content_id",    q.getColumn(25).getString()},
			{"interstitial_every_n",       q.getColumn(26).getInt()},
			{"snap_to_group_start",        q.getColumn(27).getInt() != 0},
		};
		if (!q.getColumn(4).isNull()) block["end_time"] = q.getColumn(4).getString();

		SQLite::Statement cq(db_.get(), R"(
			SELECT bc.id, bc.content_type, bc.content_id, bc.position, bc.season_filter,
			       bc.weight, bc.run_count, bc.include_specials, bc.episode_order,
			       CASE bc.content_type
			           WHEN 'show'        THEN COALESCE(sw.title,'') ||
			                                   CASE WHEN bc.season_filter IS NOT NULL
			                                        THEN ' — Season ' || bc.season_filter
			                                        ELSE '' END
			           WHEN 'episode'     THEN COALESCE(es.title,'') ||
			                                   ' S' || PRINTF('%02d',e.season) ||
			                                   'E' || PRINTF('%02d',e.episode) ||
			                                   ' — ' || COALESCE(e.title,'')
			           WHEN 'movie'       THEN COALESCE(m.title,'')
			           WHEN 'playlist'    THEN COALESCE(pl.title,'')
			           WHEN 'filler_list' THEN COALESCE(fl.title,'')
			           ELSE ''
			       END AS title
			FROM block_content bc
			LEFT JOIN show        sw ON bc.content_type = 'show'        AND bc.content_id = sw.show_id
			LEFT JOIN episode      e ON bc.content_type = 'episode'     AND bc.content_id = e.episode_id
			LEFT JOIN show        es ON bc.content_type = 'episode'     AND e.show_id = es.show_id
			LEFT JOIN movie        m ON bc.content_type = 'movie'       AND bc.content_id = m.movie_id
			LEFT JOIN playlist    pl ON bc.content_type = 'playlist'    AND bc.content_id = pl.playlist_id
			LEFT JOIN filler_list fl ON bc.content_type = 'filler_list' AND bc.content_id = fl.filler_list_id
			WHERE bc.block_id = ? ORDER BY bc.position
		)");
		cq.bind(1, bid);
		nlohmann::json content = nlohmann::json::array();
		while (cq.executeStep()) {
			nlohmann::json item = {
				{"id",               cq.getColumn(0).getInt()},
				{"content_type",     cq.getColumn(1).getString()},
				{"content_id",       cq.getColumn(2).getString()},
				{"position",         cq.getColumn(3).getInt()},
				{"weight",           cq.getColumn(5).getInt()},
				{"run_count",        cq.getColumn(6).getInt()},
				{"include_specials", cq.getColumn(7).getInt() != 0},
				{"episode_order",    cq.getColumn(8).getString()},
				{"title",            cq.getColumn(9).getString()},
			};
			if (!cq.getColumn(4).isNull()) item["season_filter"] = cq.getColumn(4).getInt();
			content.push_back(std::move(item));
		}
		block["content"] = std::move(content);

		SQLite::Statement fq(db_.get(), R"(
			SELECT bfe.id, bfe.content_type, bfe.content_id,
			       COALESCE(fl.title, pl.title, sh.title, mv.title, bfe.content_id),
			       bfe.advancement, bfe.weight, bfe.position
			FROM block_filler_entry bfe
			LEFT JOIN filler_list fl ON bfe.content_type='filler_list' AND fl.filler_list_id=bfe.content_id
			LEFT JOIN playlist    pl ON bfe.content_type='playlist'    AND pl.playlist_id=bfe.content_id
			LEFT JOIN show        sh ON bfe.content_type='show'        AND sh.show_id=bfe.content_id
			LEFT JOIN movie       mv ON bfe.content_type='movie'       AND mv.movie_id=bfe.content_id
			WHERE bfe.block_id = ? ORDER BY bfe.position
		)");
		fq.bind(1, bid);
		nlohmann::json filler_entries = nlohmann::json::array();
		while (fq.executeStep()) {
			filler_entries.push_back({
				{"id",           fq.getColumn(0).getInt()},
				{"content_type", fq.getColumn(1).getString()},
				{"content_id",   fq.getColumn(2).getString()},
				{"title",        fq.getColumn(3).getString()},
				{"advancement",  fq.getColumn(4).getString()},
				{"weight",       fq.getColumn(5).getInt()},
				{"position",     fq.getColumn(6).getInt()},
			});
		}
		block["filler_entries"] = std::move(filler_entries);

		// Slots (timeslot blocks only)
		if (q.getColumn(1).getString() == "timeslot") {
			SQLite::Statement sq(db_.get(), R"(
				SELECT slot_id, slot_index, slot_offset_mins, slot_duration_mins,
				       overflow, late_start_mins, early_start_secs, align_to_mins,
				       start_scope, queue_pos, episode_pos
				FROM timeslot_slot WHERE block_id = ? ORDER BY slot_index
			)");
			sq.bind(1, bid);
			nlohmann::json slots = nlohmann::json::array();
			while (sq.executeStep()) {
				std::string sid = sq.getColumn(0).getString();
				nlohmann::json slot = {
					{"slot_id",            sid},
					{"slot_index",         sq.getColumn(1).getInt()},
					{"slot_offset_mins",   sq.getColumn(2).getInt()},
					{"slot_duration_mins", sq.getColumn(3).getInt()},
					{"overflow",           sq.getColumn(4).getString()},
					{"late_start_mins",    sq.getColumn(5).getInt()},
					{"early_start_secs",   sq.getColumn(6).getInt()},
					{"align_to_mins",      sq.getColumn(7).getInt()},
					{"start_scope",        sq.getColumn(8).getString()},
					{"queue_pos",          sq.getColumn(9).getInt()},
					{"episode_pos",        sq.getColumn(10).getInt()},
				};
				SQLite::Statement qq(db_.get(), R"(
					SELECT entry_id, queue_index, content_type, content_id,
					       COALESCE(premiere_date,''), pre_premiere_behavior,
					       CASE content_type
					           WHEN 'show'  THEN COALESCE((SELECT title FROM show  WHERE show_id  = content_id),'')
					           WHEN 'movie' THEN COALESCE((SELECT title FROM movie WHERE movie_id = content_id),'')
					           ELSE ''
					       END
					FROM timeslot_slot_queue WHERE slot_id = ? ORDER BY queue_index
				)");
				qq.bind(1, sid);
				nlohmann::json queue_arr = nlohmann::json::array();
				while (qq.executeStep()) {
					queue_arr.push_back({
						{"entry_id",              qq.getColumn(0).getString()},
						{"queue_index",           qq.getColumn(1).getInt()},
						{"content_type",          qq.getColumn(2).getString()},
						{"content_id",            qq.getColumn(3).getString()},
						{"premiere_date",         qq.getColumn(4).getString()},
						{"pre_premiere_behavior", qq.getColumn(5).getString()},
						{"title",                 qq.getColumn(6).getString()},
					});
				}
				slot["queue"] = std::move(queue_arr);
				slots.push_back(std::move(slot));
			}
			block["slots"] = std::move(slots);
		} else {
			block["slots"] = nlohmann::json::array();
		}

		result.push_back(std::move(block));
	}
	return result;
}

// ---------------------------------------------------------------------------
// Block CRUD
// ---------------------------------------------------------------------------

std::string BlockRepository::createBlock(const std::string& channel_id,
                                          const nlohmann::json& b) {
    std::string block_id = db::generateId();
    std::string end_time = b.value("end_time", std::string(""));
    SQLite::Statement s(db_.get(), R"(
        INSERT INTO block (block_id, channel_id, name, block_type, day_mask,
                           start_time, end_time, program_count, priority,
                           play_style, advancement, cursor_scope,
                           late_start_mins, align_to_mins, inter_filler,
                           early_start_secs, filler_selection, smart_pct,
                           start_scope, no_history_behavior,
                           max_consecutive_episodes,
                           intro_content_type, intro_content_id,
                           outro_content_type, outro_content_id,
                           interstitial_content_type, interstitial_content_id,
                           interstitial_every_n, snap_to_group_start)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    )");
    s.bind(1,  block_id);
    s.bind(2,  channel_id);
    s.bind(3,  b.value("name",               std::string("")));
    s.bind(4,  b.value("block_type",         std::string("episode")));
    s.bind(5,  b.value("day_mask",           127));
    s.bind(6,  b.value("start_time",         std::string("00:00")));
    if (end_time.empty()) s.bind(7); else s.bind(7, end_time);
    s.bind(8,  b.value("program_count",      0));
    s.bind(9,  b.value("priority",           0));
    s.bind(10, b.value("play_style",         std::string("standard")));
    s.bind(11, b.value("advancement",        std::string("sequential")));
    s.bind(12, b.value("cursor_scope",       std::string("block")));
    s.bind(13, b.value("late_start_mins",    0));
    s.bind(14, b.value("align_to_mins",      0));
    s.bind(15, b.value("inter_filler", false) ? 1 : 0);
    s.bind(16, b.value("early_start_secs",   0));
    s.bind(17, b.value("filler_selection",   std::string("round_robin")));
    s.bind(18, b.value("smart_pct",          30));
    s.bind(19, b.value("start_scope",        std::string("block")));
    s.bind(20, b.value("no_history_behavior",std::string("normal")));
    s.bind(21, b.value("max_consecutive_episodes", 0));
    s.bind(22, b.value("intro_content_type",        std::string("")));
    s.bind(23, b.value("intro_content_id",          std::string("")));
    s.bind(24, b.value("outro_content_type",        std::string("")));
    s.bind(25, b.value("outro_content_id",          std::string("")));
    s.bind(26, b.value("interstitial_content_type", std::string("")));
    s.bind(27, b.value("interstitial_content_id",   std::string("")));
    s.bind(28, b.value("interstitial_every_n",      1));
    s.bind(29, b.value("snap_to_group_start", true) ? 1 : 0);
    s.exec();
    return block_id;
}

// ---------------------------------------------------------------------------
// Block content CRUD
// ---------------------------------------------------------------------------

std::pair<int64_t, int> BlockRepository::addContent(const std::string& block_id,
                                                     const nlohmann::json& b) {
    int position = 0;
    {
        SQLite::Statement pq(db_.get(),
            "SELECT COALESCE(MAX(position), -1) + 1 FROM block_content WHERE block_id = ?");
        pq.bind(1, block_id);
        if (pq.executeStep()) position = pq.getColumn(0).getInt();
    }

    SQLite::Statement s(db_.get(), R"(
        INSERT INTO block_content
            (block_id, content_type, content_id, position, season_filter, weight, run_count,
             include_specials, episode_order)
        VALUES (?,?,?,?,?,?,?,?,?)
    )");
    s.bind(1, block_id);
    s.bind(2, b.value("content_type",    std::string("show")));
    s.bind(3, b.value("content_id",      std::string("")));
    s.bind(4, position);
    if (b.contains("season_filter") && !b["season_filter"].is_null())
        s.bind(5, b["season_filter"].get<int>());
    else
        s.bind(5);
    s.bind(6, b.value("weight",          1));
    s.bind(7, b.value("run_count",       1));
    s.bind(8, b.value("include_specials", false) ? 1 : 0);
    s.bind(9, b.value("episode_order",   std::string("season")));
    s.exec();
    return {db_.get().getLastInsertRowid(), position};
}

// ---------------------------------------------------------------------------
// Block filler entry CRUD
// ---------------------------------------------------------------------------

BlockRepository::FillerEntryResult BlockRepository::addFillerEntry(
    const std::string& block_id, const nlohmann::json& b) {
    std::string content_type = b.value("content_type", std::string("filler_list"));
    std::string content_id   = b.value("content_id",   std::string(""));
    std::string advancement  = b.value("advancement",  std::string("sequential"));
    int         weight       = b.value("weight",       1);

    int position = 0;
    {
        SQLite::Statement pq(db_.get(),
            "SELECT COALESCE(MAX(position), -1) + 1 FROM block_filler_entry WHERE block_id = ?");
        pq.bind(1, block_id);
        if (pq.executeStep()) position = pq.getColumn(0).getInt();
    }

    std::string title;
    {
        const char* tsql =
            content_type == "playlist"  ? "SELECT title FROM playlist    WHERE playlist_id=?"   :
            content_type == "show"      ? "SELECT title FROM show        WHERE show_id=?"       :
            content_type == "movie"     ? "SELECT title FROM movie       WHERE movie_id=?"      :
                                          "SELECT title FROM filler_list WHERE filler_list_id=?";
        SQLite::Statement tq(db_.get(), tsql);
        tq.bind(1, content_id);
        if (tq.executeStep()) title = tq.getColumn(0).getString();
    }

    std::optional<int> season_filter;
    if (b.contains("season_filter") && !b["season_filter"].is_null())
        season_filter = b["season_filter"].get<int>();

    SQLite::Statement s(db_.get(), R"(
        INSERT INTO block_filler_entry
            (block_id, content_type, content_id, advancement, weight, position, season_filter)
        VALUES (?,?,?,?,?,?,?)
    )");
    s.bind(1, block_id); s.bind(2, content_type); s.bind(3, content_id);
    s.bind(4, advancement); s.bind(5, weight); s.bind(6, position);
    if (season_filter.has_value()) s.bind(7, season_filter.value()); else s.bind(7);
    s.exec();
    return {db_.get().getLastInsertRowid(), position, std::move(title)};
}

// ---------------------------------------------------------------------------
// Block update / remove
// ---------------------------------------------------------------------------

void BlockRepository::updateBlockField(const std::string& block_id,
                                        const std::string& col, const std::string& val) {
    SQLite::Statement s(db_.get(), "UPDATE block SET " + col + " = ? WHERE block_id = ?");
    s.bind(1, val); s.bind(2, block_id); s.exec();
}

void BlockRepository::updateBlockField(const std::string& block_id,
                                        const std::string& col, int val) {
    SQLite::Statement s(db_.get(), "UPDATE block SET " + col + " = ? WHERE block_id = ?");
    s.bind(1, val); s.bind(2, block_id); s.exec();
}

void BlockRepository::clearBlockField(const std::string& block_id, const std::string& col) {
    SQLite::Statement s(db_.get(), "UPDATE block SET " + col + " = NULL WHERE block_id = ?");
    s.bind(1, block_id); s.exec();
}

void BlockRepository::removeBlock(const std::string& block_id) {
    SQLite::Statement s(db_.get(), "DELETE FROM block WHERE block_id = ?");
    s.bind(1, block_id); s.exec();
}

// ---------------------------------------------------------------------------
// block_content update / remove
// ---------------------------------------------------------------------------

void BlockRepository::updateContentField(int id, const std::string& col, const std::string& val) {
    SQLite::Statement s(db_.get(), "UPDATE block_content SET " + col + " = ? WHERE id = ?");
    s.bind(1, val); s.bind(2, id); s.exec();
}

void BlockRepository::updateContentField(int id, const std::string& col, int val) {
    SQLite::Statement s(db_.get(), "UPDATE block_content SET " + col + " = ? WHERE id = ?");
    s.bind(1, val); s.bind(2, id); s.exec();
}

void BlockRepository::clearContentField(int id, const std::string& col) {
    SQLite::Statement s(db_.get(), "UPDATE block_content SET " + col + " = NULL WHERE id = ?");
    s.bind(1, id); s.exec();
}

void BlockRepository::removeContent(int id) {
    SQLite::Statement s(db_.get(), "DELETE FROM block_content WHERE id = ?");
    s.bind(1, id); s.exec();
}

void BlockRepository::resetContentCursor(const std::string& channel_id,
                                          const std::string& block_id,
                                          int content_row_id) {
    std::string content_type, content_id;
    {
        SQLite::Statement q(db_.get(),
            "SELECT content_type, content_id FROM block_content WHERE id = ?");
        q.bind(1, content_row_id);
        if (!q.executeStep()) throw std::runtime_error("content item not found");
        content_type = q.getColumn(0).getString();
        content_id   = q.getColumn(1).getString();
    }
    if (content_type != "show")
        throw std::invalid_argument("cursor reset only applies to show content");

    std::string play_style, cursor_scope;
    {
        SQLite::Statement q(db_.get(),
            "SELECT play_style, cursor_scope FROM block WHERE block_id = ?");
        q.bind(1, block_id);
        if (!q.executeStep()) throw std::runtime_error("block not found");
        play_style   = q.getColumn(0).getString();
        cursor_scope = q.getColumn(1).getString();
    }

    if (play_style == "rerun") {
        SQLite::Statement s(db_.get(), R"(
            DELETE FROM media_cursor
            WHERE content_type='show_rerun' AND content_id=? AND cursor_scope='block' AND scope_id=?
        )");
        s.bind(1, content_id); s.bind(2, block_id); s.exec();
    } else {
        std::string scope_id;
        if      (cursor_scope == "global")  scope_id = "";
        else if (cursor_scope == "channel") scope_id = channel_id;
        else                                scope_id = block_id;
        SQLite::Statement s(db_.get(), R"(
            DELETE FROM media_cursor
            WHERE content_type='show' AND content_id=? AND cursor_scope=? AND scope_id=?
        )");
        s.bind(1, content_id); s.bind(2, cursor_scope); s.bind(3, scope_id); s.exec();
    }
}

// ---------------------------------------------------------------------------
// block_filler_entry update / remove
// ---------------------------------------------------------------------------

void BlockRepository::updateFillerEntryField(int id, const std::string& col, const std::string& val) {
    SQLite::Statement s(db_.get(), "UPDATE block_filler_entry SET " + col + " = ? WHERE id = ?");
    s.bind(1, val); s.bind(2, id); s.exec();
}

void BlockRepository::updateFillerEntryField(int id, const std::string& col, int val) {
    SQLite::Statement s(db_.get(), "UPDATE block_filler_entry SET " + col + " = ? WHERE id = ?");
    s.bind(1, val); s.bind(2, id); s.exec();
}

void BlockRepository::removeFillerEntry(int id) {
    SQLite::Statement s(db_.get(), "DELETE FROM block_filler_entry WHERE id = ?");
    s.bind(1, id); s.exec();
}

// ---------------------------------------------------------------------------
// channel_bumper CRUD
// ---------------------------------------------------------------------------

BlockRepository::BumperResult BlockRepository::addBumper(
    const std::string& channel_id,
    const std::string& content_type, const std::string& content_id,
    const std::string& mode, int every_n,
    std::optional<int> season_filter) {
    SQLite::Statement pq(db_.get(),
        "SELECT COALESCE(MAX(position)+1,0) FROM channel_bumper WHERE channel_id=?");
    pq.bind(1, channel_id); pq.executeStep();
    int position = pq.getColumn(0).getInt();

    SQLite::Statement s(db_.get(), R"(
        INSERT INTO channel_bumper (channel_id, content_type, content_id, mode, every_n, position, season_filter)
        VALUES (?,?,?,?,?,?,?)
    )");
    s.bind(1, channel_id); s.bind(2, content_type); s.bind(3, content_id);
    s.bind(4, mode); s.bind(5, every_n); s.bind(6, position);
    if (season_filter.has_value()) s.bind(7, season_filter.value()); else s.bind(7);
    s.exec();
    int new_id = static_cast<int>(db_.get().getLastInsertRowid());

    std::string title = content_id;
    {
        const char* tsql =
            content_type == "show"     ? "SELECT title FROM show     WHERE show_id     = ? LIMIT 1" :
            content_type == "playlist" ? "SELECT title FROM playlist WHERE playlist_id = ? LIMIT 1" :
            content_type == "episode"  ? "SELECT title FROM episode  WHERE episode_id  = ? LIMIT 1" : nullptr;
        if (tsql) {
            SQLite::Statement tq(db_.get(), tsql);
            tq.bind(1, content_id);
            if (tq.executeStep()) title = tq.getColumn(0).getString();
        }
    }
    return {new_id, position, std::move(title)};
}

void BlockRepository::updateBumperField(int id, const std::string& col, const std::string& val) {
    SQLite::Statement s(db_.get(), "UPDATE channel_bumper SET " + col + " = ? WHERE id = ?");
    s.bind(1, val); s.bind(2, id); s.exec();
}

void BlockRepository::updateBumperField(int id, const std::string& col, int val) {
    SQLite::Statement s(db_.get(), "UPDATE channel_bumper SET " + col + " = ? WHERE id = ?");
    s.bind(1, val); s.bind(2, id); s.exec();
}

void BlockRepository::removeBumper(int id) {
    SQLite::Statement s(db_.get(), "DELETE FROM channel_bumper WHERE id = ?");
    s.bind(1, id); s.exec();
}

// ---------------------------------------------------------------------------
// Episode group CRUD
// ---------------------------------------------------------------------------

std::string BlockRepository::createEpisodeGroup(const std::string& show_id,
                                                  const std::string& name,
                                                  const std::string& group_type) {
    std::string group_id = db::generateId();
    SQLite::Statement s(db_.get(),
        "INSERT INTO episode_group (group_id, show_id, name, group_type) VALUES (?,?,?,?)");
    s.bind(1, group_id); s.bind(2, show_id); s.bind(3, name); s.bind(4, group_type); s.exec();
    return group_id;
}

void BlockRepository::removeEpisodeGroup(const std::string& group_id) {
    SQLite::Statement s(db_.get(), "DELETE FROM episode_group WHERE group_id = ?");
    s.bind(1, group_id); s.exec();
}

std::pair<int64_t, int> BlockRepository::addGroupMember(const std::string& group_id,
                                                          const std::string& episode_id,
                                                          int part_num) {
    SQLite::Statement s(db_.get(),
        "INSERT INTO episode_group_member (group_id, episode_id, part_num) VALUES (?,?,?)");
    s.bind(1, group_id); s.bind(2, episode_id); s.bind(3, part_num); s.exec();
    return {db_.get().getLastInsertRowid(), part_num};
}

void BlockRepository::removeGroupMember(int member_id) {
    SQLite::Statement s(db_.get(), "DELETE FROM episode_group_member WHERE id = ?");
    s.bind(1, member_id); s.exec();
}
