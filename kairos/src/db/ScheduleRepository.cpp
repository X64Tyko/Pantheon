#include "ScheduleRepository.h"
#include "Database.h"
#include "DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>

ScheduleRepository::ScheduleRepository(Database& db) : db_(db) {}

std::optional<NowProgramRow>
ScheduleRepository::getNowProgram(const std::string& channel_id, int64_t at_sec) {
    SQLite::Statement q(db_.get(), R"(
        SELECT sp.item_type, sp.item_id,
               COALESCE(sp.block_id, '')              AS block_id,
               sp.wall_clock_start,
               COALESCE(e.title,  m.title,  '')       AS title,
               COALESCE(s.title,  '')                 AS show_title,
               COALESCE(e.show_id,'')                 AS show_id,
               COALESCE(e.season, 0)                  AS season,
               COALESCE(e.episode,0)                  AS ep_num,
               COALESCE(e.file_path, m.file_path,'')  AS file_path,
               COALESCE(e.duration_ms, m.duration_ms, 0) AS duration_ms,
               sp.wall_clock_end,
               sp.is_filler
        FROM scheduled_program sp
        LEFT JOIN episode e ON sp.item_type='episode' AND sp.item_id=e.episode_id
        LEFT JOIN show    s ON sp.item_type='episode' AND e.show_id =s.show_id
        LEFT JOIN movie   m ON sp.item_type='movie'   AND sp.item_id=m.movie_id
        WHERE sp.channel_id    = ?
          AND sp.wall_clock_start <= ?
          AND sp.wall_clock_end   >  ?
          AND sp.status != 'skipped'
        ORDER BY sp.wall_clock_start DESC
        LIMIT 1
    )");
    q.bind(1, channel_id);
    q.bind(2, at_sec);
    q.bind(3, at_sec);
    if (!q.executeStep()) return std::nullopt;

    NowProgramRow r;
    r.item_type        = q.getColumn(0).getString();
    r.item_id          = q.getColumn(1).getString();
    r.block_id         = q.getColumn(2).getString();
    r.wall_clock_start = q.getColumn(3).getInt64();
    r.title            = q.getColumn(4).getString();
    r.show_title       = q.getColumn(5).getString();
    r.show_id          = q.getColumn(6).getString();
    r.season           = q.getColumn(7).getInt();
    r.episode          = q.getColumn(8).getInt();
    r.file_path        = q.getColumn(9).getString();
    r.duration_ms      = q.getColumn(10).getInt64();
    r.wall_clock_end   = q.getColumn(11).getInt64();
    r.is_filler        = q.getColumn(12).getInt() != 0;
    return r;
}

std::optional<FillerFallbackRow>
ScheduleRepository::getChannelFillerFallback(const std::string& channel_id) {
    // Prefer the least-recently-played eligible clip (never-played first, tie-broken
    // by RANDOM()) instead of a bare RANDOM() pick. Plain RANDOM() has no memory of
    // what just aired, so callers that re-query on every filler-to-filler transition
    // (Hephaestus does this once per short filler clip) would otherwise reroll the
    // same small pool constantly and repeat clips back-to-back.
    SQLite::Statement q(db_.get(), R"(
        SELECT fi.item_type, fi.item_id,
               COALESCE(e.file_path, m.file_path, '') AS file_path,
               COALESCE(e.title,     m.title,     '') AS title,
               COALESCE(e.duration_ms, m.duration_ms, 0) AS duration_ms
        FROM channel_filler_entry cfe
        JOIN filler_list_item fi ON fi.filler_list_id = cfe.filler_list_id
        LEFT JOIN episode e ON fi.item_type='episode' AND fi.item_id=e.episode_id
        LEFT JOIN movie   m ON fi.item_type='movie'   AND fi.item_id=m.movie_id
        LEFT JOIN (
            SELECT item_id, MAX(aired_at) AS last_aired
            FROM play_history
            WHERE channel_id = ?
            GROUP BY item_id
        ) ph ON ph.item_id = fi.item_id
        WHERE cfe.channel_id = ?
          AND (e.file_path IS NOT NULL OR m.file_path IS NOT NULL)
        ORDER BY COALESCE(ph.last_aired, 0) ASC, RANDOM()
        LIMIT 1
    )");
    q.bind(1, channel_id);
    q.bind(2, channel_id);
    if (!q.executeStep()) return std::nullopt;

    FillerFallbackRow r;
    r.item_type   = q.getColumn(0).getString();
    r.item_id     = q.getColumn(1).getString();
    r.file_path   = q.getColumn(2).getString();
    r.title       = q.getColumn(3).getString();
    r.duration_ms = q.getColumn(4).getInt64();

    recordPlayHistory(r.item_type, r.item_id, channel_id, "");
    return r;
}

std::optional<OfflineFallbackRow>
ScheduleRepository::getChannelOfflineConfig(const std::string& channel_id) {
    SQLite::Statement q(db_.get(),
        "SELECT offline_video_path, offline_image_path, "
        "       offline_audio_id, offline_audio_type "
        "FROM channel WHERE channel_id = ?");
    q.bind(1, channel_id);
    if (!q.executeStep()) return std::nullopt;

    OfflineFallbackRow r;
    r.vid_path  = q.getColumn(0).getString();
    r.img_path  = q.getColumn(1).getString();
    r.audio_id  = q.getColumn(2).getString();
    r.audio_typ = q.getColumn(3).getString();
    return r;
}

std::optional<std::string>
ScheduleRepository::getAudioFilePath(const std::string& item_type, const std::string& item_id) {
    const char* sql = (item_type == "episode")
        ? "SELECT file_path FROM episode WHERE episode_id = ?"
        : "SELECT file_path FROM movie   WHERE movie_id   = ?";
    SQLite::Statement q(db_.get(), sql);
    q.bind(1, item_id);
    if (!q.executeStep()) return std::nullopt;
    auto p = q.getColumn(0).getString();
    return p.empty() ? std::nullopt : std::optional<std::string>(p);
}

std::optional<NextProgramRow>
ScheduleRepository::getNextProgram(const std::string& channel_id, int64_t now_sec) {
    int64_t current_end = now_sec;
    {
        SQLite::Statement q(db_.get(), R"(
            SELECT wall_clock_end FROM scheduled_program
            WHERE channel_id = ? AND wall_clock_start <= ? AND wall_clock_end > ?
              AND status != 'skipped'
            ORDER BY wall_clock_start DESC LIMIT 1
        )");
        q.bind(1, channel_id);
        q.bind(2, now_sec);
        q.bind(3, now_sec);
        if (q.executeStep()) current_end = q.getColumn(0).getInt64();
    }

    SQLite::Statement q(db_.get(), R"(
        SELECT sp.item_type, sp.item_id, COALESCE(sp.block_id, ''),
               sp.wall_clock_start,
               COALESCE(e.title, m.title, '')       AS title,
               COALESCE(s.title, '')                AS show_title,
               COALESCE(e.show_id, '')              AS show_id,
               COALESCE(e.season,  0)               AS season,
               COALESCE(e.episode, 0)               AS ep_num,
               COALESCE(e.file_path, m.file_path, '') AS file_path,
               COALESCE(e.duration_ms, m.duration_ms,
                        (sp.wall_clock_end - sp.wall_clock_start) * 1000) AS duration_ms
        FROM scheduled_program sp
        LEFT JOIN episode e ON sp.item_type = 'episode' AND sp.item_id = e.episode_id
        LEFT JOIN show    s ON sp.item_type = 'episode' AND e.show_id  = s.show_id
        LEFT JOIN movie   m ON sp.item_type = 'movie'   AND sp.item_id = m.movie_id
        WHERE sp.channel_id = ?
          AND sp.wall_clock_start >= ?
          AND sp.is_filler = 0
          AND sp.status != 'skipped'
        ORDER BY sp.wall_clock_start
        LIMIT 1
    )");
    q.bind(1, channel_id);
    q.bind(2, current_end);
    if (!q.executeStep()) return std::nullopt;

    NextProgramRow r;
    r.item_type        = q.getColumn(0).getString();
    r.item_id          = q.getColumn(1).getString();
    r.block_id         = q.getColumn(2).getString();
    r.wall_clock_start = q.getColumn(3).getInt64();
    r.title            = q.getColumn(4).getString();
    r.show_title       = q.getColumn(5).getString();
    r.show_id          = q.getColumn(6).getString();
    r.season           = q.getColumn(7).getInt();
    r.episode          = q.getColumn(8).getInt();
    r.file_path        = q.getColumn(9).getString();
    r.duration_ms      = q.getColumn(10).getInt64();
    return r;
}

std::vector<EpgProgramRow>
ScheduleRepository::getEpgPrograms(const std::string& channel_id,
                                    int64_t from_sec, int64_t to_sec) {
    // Filler items are excluded from the live EPG response. Each content item's
    // wall_clock_end is extended to the next content item's start (absorbing the
    // filler gap), matching the XMLTV output. The +7200s cap prevents runaway
    // expansion across long inter-block gaps.
    SQLite::Statement q(db_.get(), R"(
        WITH content AS (
            SELECT sp.item_type, sp.item_id, COALESCE(sp.block_id, '') AS block_id,
                   sp.wall_clock_start, sp.wall_clock_end, sp.status,
                   LEAD(sp.wall_clock_start) OVER (
                       PARTITION BY sp.channel_id ORDER BY sp.wall_clock_start
                   ) AS next_content_start
            FROM scheduled_program sp
            WHERE sp.channel_id = ?
              AND sp.is_filler  = 0
              AND sp.wall_clock_end   >  ?
              AND sp.wall_clock_start <  ?
              AND sp.status != 'skipped'
        )
        SELECT c.item_type, c.item_id, c.block_id,
               c.wall_clock_start,
               CASE WHEN c.next_content_start IS NOT NULL
                         AND c.next_content_start <= c.wall_clock_end + 7200
                    THEN c.next_content_start
                    ELSE c.wall_clock_end
               END AS wall_clock_end,
               c.status,
               COALESCE(e.title, m.title, '')         AS item_title,
               COALESCE(s.title, '')                  AS show_title,
               COALESCE(e.show_id, '')                AS show_id,
               COALESCE(e.season,  0)                 AS season,
               COALESCE(e.episode, 0)                 AS ep_num,
               COALESCE(e.file_path, m.file_path, '') AS file_path,
               COALESCE(e.duration_ms, m.duration_ms,
                        (c.wall_clock_end - c.wall_clock_start) * 1000) AS duration_ms
        FROM content c
        LEFT JOIN episode e ON c.item_type = 'episode' AND c.item_id = e.episode_id
        LEFT JOIN show    s ON c.item_type = 'episode' AND e.show_id  = s.show_id
        LEFT JOIN movie   m ON c.item_type = 'movie'   AND c.item_id  = m.movie_id
        ORDER BY c.wall_clock_start
    )");
    q.bind(1, channel_id);
    q.bind(2, from_sec);
    q.bind(3, to_sec);

    std::vector<EpgProgramRow> rows;
    while (q.executeStep()) {
        EpgProgramRow r;
        r.item_type        = q.getColumn(0).getString();
        r.item_id          = q.getColumn(1).getString();
        r.block_id         = q.getColumn(2).getString();
        r.wall_clock_start = q.getColumn(3).getInt64();
        r.wall_clock_end   = q.getColumn(4).getInt64();
        r.status           = q.getColumn(5).getString();
        r.title            = q.getColumn(6).getString();
        r.show_title       = q.getColumn(7).getString();
        r.show_id          = q.getColumn(8).getString();
        r.season           = q.getColumn(9).getInt();
        r.episode          = q.getColumn(10).getInt();
        r.file_path        = q.getColumn(11).getString();
        r.duration_ms      = q.getColumn(12).getInt64();
        rows.push_back(std::move(r));
    }
    return rows;
}

int ScheduleRepository::clearAllScheduled() {
    SQLite::Statement q(db_.get(), "DELETE FROM scheduled_program");
    return q.exec();
}

void ScheduleRepository::insertPreviewBlocks(const std::string& channel_id,
                                              const nlohmann::json& blocks) {
    using json = nlohmann::json;

    SQLite::Statement delbc(db_.get(),
        "DELETE FROM block_content WHERE block_id IN (SELECT block_id FROM block WHERE channel_id=?)");
    delbc.bind(1, channel_id); delbc.exec();
    SQLite::Statement delbfe(db_.get(),
        "DELETE FROM block_filler_entry WHERE block_id IN (SELECT block_id FROM block WHERE channel_id=?)");
    delbfe.bind(1, channel_id); delbfe.exec();
    SQLite::Statement delb(db_.get(), "DELETE FROM block WHERE channel_id=?");
    delb.bind(1, channel_id); delb.exec();

    for (const auto& blk : blocks) {
        std::string block_id = blk.value("block_id", db::generateId());
        std::string end_time = blk.value("end_time", "");
        SQLite::Statement s(db_.get(), R"(
            INSERT INTO block (block_id, channel_id, name, block_type, day_mask,
                               start_time, end_time, program_count, priority,
                               play_style, advancement, cursor_scope,
                               late_start_mins, align_to_mins, inter_filler,
                               early_start_secs, filler_selection, smart_pct,
                               start_scope, no_history_behavior,
                               max_consecutive_episodes, interstitial_every_n)
            VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        )");
        s.bind(1, block_id); s.bind(2, channel_id);
        s.bind(3, blk.value("name", ""));
        s.bind(4, blk.value("block_type", "episode"));
        s.bind(5, blk.value("day_mask", 127));
        s.bind(6, blk.value("start_time", "00:00"));
        if (end_time.empty()) s.bind(7); else s.bind(7, end_time);
        s.bind(8,  blk.value("program_count",           0));
        s.bind(9,  blk.value("priority",                0));
        s.bind(10, blk.value("play_style",              std::string("standard")));
        s.bind(11, blk.value("advancement",             std::string("sequential")));
        s.bind(12, blk.value("cursor_scope",            std::string("block")));
        s.bind(13, blk.value("late_start_mins",         0));
        s.bind(14, blk.value("align_to_mins",           0));
        s.bind(15, blk.value("inter_filler", false) ? 1 : 0);
        s.bind(16, blk.value("early_start_secs",        0));
        s.bind(17, blk.value("filler_selection",        std::string("round_robin")));
        s.bind(18, blk.value("smart_pct",               30));
        s.bind(19, blk.value("start_scope",             std::string("block")));
        s.bind(20, blk.value("no_history_behavior",     std::string("normal")));
        s.bind(21, blk.value("max_consecutive_episodes",0));
        s.bind(22, blk.value("interstitial_every_n",    1));
        s.exec();

        int pos = 0;
        for (const auto& item : blk.value("content", json::array())) {
            std::string cid = item.value("content_id",   "");
            std::string ct  = item.value("content_type", "");
            if (cid.empty() || ct.empty()) { pos++; continue; }
            SQLite::Statement ins(db_.get(), R"(
                INSERT OR IGNORE INTO block_content
                    (block_id, content_type, content_id, position,
                     weight, run_count, include_specials, episode_order)
                VALUES (?,?,?,?,?,?,?,?)
            )");
            ins.bind(1, block_id); ins.bind(2, ct); ins.bind(3, cid);
            ins.bind(4, pos);
            ins.bind(5, item.value("weight",           1));
            ins.bind(6, item.value("run_count",        1));
            ins.bind(7, item.value("include_specials", false) ? 1 : 0);
            ins.bind(8, item.value("episode_order",    std::string("season")));
            ins.exec();
            if (item.contains("season_filter") && !item["season_filter"].is_null()) {
                SQLite::Statement upd(db_.get(), R"(
                    UPDATE block_content SET season_filter = ?
                    WHERE block_id = ? AND content_type = ? AND content_id = ?
                )");
                upd.bind(1, item["season_filter"].get<int>());
                upd.bind(2, block_id); upd.bind(3, ct); upd.bind(4, cid);
                upd.exec();
            }
            pos++;
        }

        int fpos = 0;
        for (const auto& fe : blk.value("filler_entries", json::array())) {
            std::string ct  = fe.value("content_type", "filler_list");
            std::string cid = fe.value("content_id",   fe.value("filler_list_id", ""));
            if (cid.empty()) { fpos++; continue; }
            SQLite::Statement ins(db_.get(), R"(
                INSERT OR IGNORE INTO block_filler_entry
                    (block_id, content_type, content_id, advancement, weight, position)
                VALUES (?,?,?,?,?,?)
            )");
            ins.bind(1, block_id); ins.bind(2, ct); ins.bind(3, cid);
            ins.bind(4, fe.value("advancement", std::string("sized")));
            ins.bind(5, fe.value("weight", 1));
            ins.bind(6, fpos); ins.exec();
            fpos++;
        }

        if (blk.value("block_type", std::string("")) == "timeslot") {
            int sidx = 0;
            for (const auto& slot : blk.value("slots", json::array())) {
                std::string slot_id = slot.value("slot_id", db::generateId());
                SQLite::Statement ss(db_.get(), R"(
                    INSERT OR IGNORE INTO timeslot_slot
                        (slot_id, block_id, slot_index, slot_offset_mins,
                         slot_duration_mins, overflow, late_start_mins,
                         early_start_secs, align_to_mins, start_scope)
                    VALUES (?,?,?,?,?,?,?,?,?,?)
                )");
                ss.bind(1, slot_id); ss.bind(2, block_id);
                ss.bind(3, sidx++);
                ss.bind(4,  slot.value("slot_offset_mins",   0));
                ss.bind(5,  slot.value("slot_duration_mins", 60));
                ss.bind(6,  slot.value("overflow",           std::string("cutoff")));
                ss.bind(7,  slot.value("late_start_mins",    5));
                ss.bind(8,  slot.value("early_start_secs",   0));
                ss.bind(9,  slot.value("align_to_mins",      0));
                ss.bind(10, slot.value("start_scope",        std::string("block")));
                ss.exec();

                int qidx = 0;
                for (const auto& qe : slot.value("queue", json::array())) {
                    std::string entry_id = qe.value("entry_id", db::generateId());
                    std::string prem     = qe.value("premiere_date", std::string(""));
                    SQLite::Statement sq(db_.get(), R"(
                        INSERT OR IGNORE INTO timeslot_slot_queue
                            (entry_id, slot_id, queue_index, content_type, content_id,
                             premiere_date, pre_premiere_behavior)
                        VALUES (?,?,?,?,?,?,?)
                    )");
                    sq.bind(1, entry_id); sq.bind(2, slot_id); sq.bind(3, qidx++);
                    sq.bind(4, qe.value("content_type", std::string("show")));
                    sq.bind(5, qe.value("content_id",   std::string("")));
                    if (prem.empty()) sq.bind(6); else sq.bind(6, prem);
                    sq.bind(7, qe.value("pre_premiere_behavior", std::string("replay_previous")));
                    sq.exec();
                }
            }
        }
    }
}

void ScheduleRepository::recordPlayHistory(const std::string& item_type,
                                            const std::string& item_id,
                                            const std::string& channel_id,
                                            const std::string& block_id) {
    SQLite::Statement q(db_.get(), R"(
        INSERT INTO play_history (item_type, item_id, channel_id, block_id, aired_at, is_scheduled)
        VALUES (?,?,?,?,?,0)
    )");
    q.bind(1, item_type); q.bind(2, item_id); q.bind(3, channel_id);
    if (block_id.empty()) q.bind(4); else q.bind(4, block_id);
    q.bind(5, static_cast<int64_t>(std::time(nullptr)));
    q.exec();
}

void ScheduleRepository::recordScheduledPlayHistory(const std::string& item_type,
                                                     const std::string& item_id,
                                                     const std::string& channel_id,
                                                     const std::string& block_id,
                                                     std::time_t aired_at) {
    SQLite::Statement q(db_.get(), R"(
        INSERT OR IGNORE INTO play_history (item_type, item_id, channel_id, block_id, aired_at, is_scheduled)
        VALUES (?,?,?,?,?,1)
    )");
    q.bind(1, item_type); q.bind(2, item_id); q.bind(3, channel_id);
    if (block_id.empty()) q.bind(4); else q.bind(4, block_id);
    q.bind(5, static_cast<int64_t>(aired_at));
    q.exec();
}
