#include "EPGMaterializer.h"
#include "Rng.h"
#include "../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>

static bool epgDebug() {
    static const bool v = [] {
        const char* e = std::getenv("KAIROS_DEBUG_EPG");
        return e && std::string(e) == "1";
    }();
    return v;
}

EPGMaterializer::EPGMaterializer(Database& db, RuleEngine& engine)
    : db_(db), engine_(engine) {}

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string EPGMaterializer::xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            default:  out += c;
        }
    }
    return out;
}

std::string EPGMaterializer::fmtXMLTVTime(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d%H%M%S +0000", &tm);
    return buf;
}

// ── Schedule cache management ─────────────────────────────────────────────────

void EPGMaterializer::ensureScheduled(const std::string& channel_id,
                                       std::time_t from, int horizon_hours,
                                       int seed) {
    // ── on_play mode: cursors advance only on confirmed playback. ─────────────
    // project() runs inside a SAVEPOINT so its cursor/history writes are rolled
    // back; only the generated scheduled_program rows are committed. The EPG is
    // always regenerated from the current cursor position (no caching between plays).
    {
        bool on_play = false;
        {
            SQLite::Statement q(db_.get(), "SELECT advance_mode FROM channel WHERE channel_id=?");
            q.bind(1, channel_id);
            if (q.executeStep()) on_play = (q.getColumn(0).getString() == "on_play");
        }
        if (on_play) {
            auto now_ts = static_cast<int64_t>(std::time(nullptr));

            // Clear the stale schedule and unconfirmed history — the EPG always
            // reflects the current cursor position, not a clock-driven plan.
            {
                SQLite::Statement d1(db_.get(),
                    "DELETE FROM scheduled_program WHERE channel_id=?");
                d1.bind(1, channel_id); d1.exec();
            }
            {
                SQLite::Statement d2(db_.get(),
                    "DELETE FROM play_history WHERE channel_id=? AND is_scheduled=1");
                d2.bind(1, channel_id); d2.exec();
            }

            // Generate items with project() inside a SAVEPOINT — cursor advances
            // and play_history writes are rolled back; items stay in memory.
            db_.get().exec("SAVEPOINT onplay_gen_sp");
            Xoshiro256 onplay_rng(seed >= 0 ? static_cast<uint64_t>(seed) : 0);
            auto items = engine_.project(channel_id, from, horizon_hours, onplay_rng);
            db_.get().exec("ROLLBACK TO SAVEPOINT onplay_gen_sp");
            db_.get().exec("RELEASE SAVEPOINT onplay_gen_sp");

            // Commit only the scheduled_program entries (including filler blocks).
            db_.get().exec("SAVEPOINT sp_ens");
            SQLite::Statement ins(db_.get(), R"(
                INSERT OR IGNORE INTO scheduled_program
                    (channel_id, block_id, item_type, item_id,
                     wall_clock_start, wall_clock_end, cursor_json, created_at, is_filler)
                VALUES (?,?,?,?,?,?,?,?,?)
            )");
            for (const auto& item : items) {
                ins.bind(1, channel_id);
                if (item.block_id.empty()) ins.bind(2); else ins.bind(2, item.block_id);
                ins.bind(3, item.item_type);
                ins.bind(4, item.item_id);
                ins.bind(5, item.wall_clock_start_ms / 1000);
                ins.bind(6, item.wall_clock_end_ms   / 1000);
                ins.bind(7, item.cursor_json);
                ins.bind(8, now_ts);
                ins.bind(9, item.is_filler ? 1 : 0);
                ins.exec();
                ins.reset();
            }
            db_.get().exec("RELEASE SAVEPOINT sp_ens");

            if (epgDebug())
                std::cout << "[epg] ensureScheduled on_play channel=" << channel_id
                          << " => " << items.size() << " items projected\n";
            return;
        }
    }
    // ── scheduled mode (default) ──────────────────────────────────────────────

    std::time_t horizon = from + static_cast<std::time_t>(horizon_hours) * 3600;
    auto now = static_cast<int64_t>(std::time(nullptr));

    if (epgDebug())
        std::cout << "[epg] ensureScheduled channel=" << channel_id
                  << " from=" << from << " horizon_hours=" << horizon_hours << '\n';

    // Resolve seed (explicit arg wins; otherwise read from channel table).
    int init_seed = seed;
    if (init_seed < 0) {
        SQLite::Statement qs(db_.get(),
            "SELECT seed FROM channel WHERE channel_id=?");
        qs.bind(1, channel_id);
        if (qs.executeStep()) init_seed = qs.getColumn(0).getInt();
    }

    // Monday midnight UTC for the week that contains `from`.
    const std::time_t from_days        = from / 86400;
    const std::time_t from_dow         = (from_days + 3) % 7;  // 0 = Mon
    const std::time_t from_week_monday = (from_days - from_dow) * 86400;

    // Try to restore RNG from the stored anchor for this week; fall back to channel seed.
    Xoshiro256 rng(init_seed >= 0 ? static_cast<uint64_t>(init_seed) : 0ULL);
    {
        using json = nlohmann::json;
        SQLite::Statement qa(db_.get(),
            "SELECT anchor_hashes FROM channel WHERE channel_id=?");
        qa.bind(1, channel_id);
        if (qa.executeStep() && !qa.getColumn(0).isNull()) {
            try {
                auto aj  = json::parse(qa.getColumn(0).getString());
                auto key = std::to_string(from_week_monday);
                if (aj.contains(key) && aj[key].is_object() && aj[key].contains("rng"))
                    rng = Xoshiro256::deserialize(aj[key]["rng"].get<std::string>());
            } catch (...) {}
        }
    }

    std::map<std::time_t, std::string> new_anchors;

    // Loop until the cache fully covers the horizon. project() is capped at
    // MAX_ITEMS per call; for channels with short clips this loop re-extends
    // from the tail on each pass rather than leaving a gap.
    for (int guard = 0; guard < 200; ++guard) {
        // Re-query the tail each pass — the previous pass may have added rows.
        std::time_t last_end = 0;
        {
            SQLite::Statement q(db_.get(), R"(
                SELECT wall_clock_end FROM scheduled_program
                WHERE channel_id = ?
                ORDER BY wall_clock_end DESC LIMIT 1
            )");
            q.bind(1, channel_id);
            if (q.executeStep())
                last_end = static_cast<std::time_t>(q.getColumn(0).getInt64());
        }

        if (epgDebug())
            std::cout << "[epg]   guard=" << guard
                      << " last_end=" << last_end << " horizon=" << horizon << '\n';

        if (last_end >= horizon) {
            if (epgDebug()) std::cout << "[epg]   cache fully covers horizon — done\n";
            return;
        }

        std::time_t extend_from = (last_end > from) ? last_end : from;
        int extend_hours = static_cast<int>((horizon - extend_from) / 3600) + 2;

        if (epgDebug())
            std::cout << "[epg]   calling project() extend_from=" << extend_from
                      << " extend_hours=" << extend_hours << '\n';

        // project() writes play_history (is_scheduled=1) and advances DB cursors
        // as it schedules each item; DB state is the authoritative resume point.
        auto items = engine_.project(channel_id, extend_from, extend_hours, rng, &new_anchors);

        if (epgDebug())
            std::cout << "[epg]   project() returned " << items.size() << " items\n";

        if (items.empty()) {
            std::cout << "[epg] WARNING: project() returned 0 items for channel="
                      << channel_id << " — EPG will be empty\n";
            return;
        }

        db_.get().exec("SAVEPOINT sp_ens");
        SQLite::Statement ins(db_.get(), R"(
            INSERT OR IGNORE INTO scheduled_program
                (channel_id, block_id, item_type, item_id,
                 wall_clock_start, wall_clock_end, cursor_json, created_at, is_filler)
            VALUES (?,?,?,?,?,?,?,?,?)
        )");

        bool any_new = false;
        int inserted = 0, skipped_dup = 0, skipped_horizon = 0;
        for (const auto& item : items) {
            std::time_t item_end = item.wall_clock_end_ms / 1000;
            if (item_end > horizon + 7200) { ++skipped_horizon; break; }

            ins.bind(1, channel_id);
            if (item.block_id.empty()) ins.bind(2); else ins.bind(2, item.block_id);
            ins.bind(3, item.item_type);
            ins.bind(4, item.item_id);
            ins.bind(5, item.wall_clock_start_ms / 1000);
            ins.bind(6, item_end);
            ins.bind(7, item.cursor_json);
            ins.bind(8, now);
            ins.bind(9, item.is_filler ? 1 : 0);
            ins.exec();
            if (db_.get().getChanges() > 0) { any_new = true; ++inserted; }
            else ++skipped_dup;
            ins.reset();
        }
        db_.get().exec("RELEASE SAVEPOINT sp_ens");

        if (epgDebug())
            std::cout << "[epg]   inserted=" << inserted
                      << " dup_skipped=" << skipped_dup
                      << " horizon_skipped=" << skipped_horizon
                      << " any_new=" << any_new << '\n';

        if (!any_new) {
            if (epgDebug())
                std::cout << "[epg]   all items already cached — done\n";
            return;
        }
    }

    // Persist any week-boundary anchor snapshots captured during projection.
    // In the preview SAVEPOINT path this UPDATE is rolled back automatically —
    // no special flag needed; correct behaviour falls out of the SAVEPOINT.
    if (!new_anchors.empty()) {
        using json = nlohmann::json;
        json existing = json::object();
        {
            SQLite::Statement qa(db_.get(),
                "SELECT anchor_hashes FROM channel WHERE channel_id=?");
            qa.bind(1, channel_id);
            if (qa.executeStep() && !qa.getColumn(0).isNull()) {
                try { existing = json::parse(qa.getColumn(0).getString()); } catch (...) {}
            }
        }
        for (auto& [ts, snap_str] : new_anchors) {
            try { existing[std::to_string(ts)] = json::parse(snap_str); } catch (...) {}
        }
        SQLite::Statement upd(db_.get(),
            "UPDATE channel SET anchor_hashes=? WHERE channel_id=?");
        upd.bind(1, existing.dump());
        upd.bind(2, channel_id);
        upd.exec();
    }
}

void EPGMaterializer::notifyPlayed(const std::string& channel_id,
                                    const std::string& item_id) {
    // Mark the earliest scheduled occurrence of this item as aired.
    SQLite::Statement q(db_.get(), R"(
        UPDATE scheduled_program SET status = 'aired'
        WHERE id = (
            SELECT id FROM scheduled_program
            WHERE channel_id = ? AND item_id = ? AND status = 'scheduled'
            ORDER BY wall_clock_start ASC LIMIT 1
        )
    )");
    q.bind(1, channel_id);
    q.bind(2, item_id);
    q.exec();
}

// ── XMLTV ─────────────────────────────────────────────────────────────────────

std::string EPGMaterializer::generateXMLTV(int horizon_hours) {
    struct Chan { std::string id, name; int number; };
    std::vector<Chan> channels;
    {
        SQLite::Statement q(db_.get(),
            "SELECT channel_id, name, number FROM channel ORDER BY number");
        while (q.executeStep())
            channels.push_back({ q.getColumn(0).getString(),
                                  q.getColumn(1).getString(),
                                  q.getColumn(2).getInt() });
    }

    std::time_t now     = std::time(nullptr);
    std::time_t horizon = now + static_cast<std::time_t>(horizon_hours) * 3600;

    if (epgDebug())
        std::cout << "[epg] generateXMLTV: " << channels.size()
                  << " channel(s), horizon_hours=" << horizon_hours << '\n';

    // Extend cache for every channel before building the XML.
    for (const auto& ch : channels)
        ensureScheduled(ch.id, now, horizon_hours);

    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n"
        << "<tv source-info-name=\"Kairos\" generator-info-name=\"Kairos\">\n";

    for (const auto& ch : channels) {
        xml << "  <channel id=\"kairos-" << ch.number << "\">\n"
            << "    <display-name>" << xmlEscape(ch.name) << "</display-name>\n"
            << "  </channel>\n";
    }

    for (const auto& ch : channels) {
        // Filler items (is_filler=1) are excluded from XMLTV output; instead,
        // each content item's stop time is extended to the next content item's
        // start time (absorbing the filler gap). LEAD() finds that next start.
        // The +7200s cap prevents runaway expansion across long inter-block gaps.
        SQLite::Statement q(db_.get(), R"(
            WITH content AS (
                SELECT sp.item_type, sp.item_id,
                       sp.wall_clock_start,
                       sp.wall_clock_end,
                       LEAD(sp.wall_clock_start) OVER (
                           PARTITION BY sp.channel_id ORDER BY sp.wall_clock_start
                       ) AS next_content_start
                FROM scheduled_program sp
                WHERE sp.channel_id = ?
                  AND sp.is_filler = 0
                  AND sp.wall_clock_end   >  ?
                  AND sp.wall_clock_start <  ?
                  AND sp.status != 'skipped'
            )
            SELECT c.item_type,
                   c.wall_clock_start,
                   CASE WHEN c.next_content_start IS NOT NULL
                             AND c.next_content_start <= c.wall_clock_end + 7200
                        THEN c.next_content_start
                        ELSE c.wall_clock_end
                   END AS stop_time,
                   COALESCE(e.title,    m.title,    '')  AS item_title,
                   COALESCE(s.title,    '')              AS show_title,
                   COALESCE(e.season,   0)               AS season,
                   COALESCE(e.episode,  0)               AS ep_num,
                   COALESCE(e.overview, m.overview, '')  AS description
            FROM content c
            LEFT JOIN episode e ON c.item_type = 'episode' AND c.item_id = e.episode_id
            LEFT JOIN show    s ON c.item_type = 'episode' AND e.show_id  = s.show_id
            LEFT JOIN movie   m ON c.item_type = 'movie'   AND c.item_id = m.movie_id
            ORDER BY c.wall_clock_start
        )");
        q.bind(1, ch.id);
        q.bind(2, static_cast<int64_t>(now));
        q.bind(3, static_cast<int64_t>(horizon));

        int prog_count = 0;
        while (q.executeStep()) {
            ++prog_count;
            std::time_t start       = static_cast<std::time_t>(q.getColumn(1).getInt64());
            std::time_t stop        = static_cast<std::time_t>(q.getColumn(2).getInt64());
            std::string item_title  = q.getColumn(3).getString();
            std::string show_title  = q.getColumn(4).getString();
            int         season      = q.getColumn(5).getInt();
            int         ep_num      = q.getColumn(6).getInt();
            std::string description = q.getColumn(7).getString();

            std::string display_title = show_title.empty() ? item_title : show_title;

            xml << "  <programme"
                << " start=\"" << fmtXMLTVTime(start) << "\""
                << " stop=\""  << fmtXMLTVTime(stop)  << "\""
                << " channel=\"kairos-" << ch.number << "\">\n"
                << "    <title lang=\"en\">" << xmlEscape(display_title) << "</title>\n";

            if (!show_title.empty()) {
                xml << "    <sub-title lang=\"en\">"
                    << xmlEscape(item_title) << "</sub-title>\n";
                if (season > 0 && ep_num > 0) {
                    xml << "    <episode-num system=\"xmltv_ns\">"
                        << (season - 1) << "." << (ep_num - 1) << ".0/1"
                        << "</episode-num>\n"
                        << "    <episode-num system=\"onscreen\">"
                        << "S" << season << "E" << ep_num
                        << "</episode-num>\n";
                }
            }

            if (!description.empty())
                xml << "    <desc lang=\"en\">" << xmlEscape(description) << "</desc>\n";

            xml << "  </programme>\n";
        }
        if (epgDebug())
            std::cout << "[epg]   XMLTV channel=" << ch.id
                      << " programmes=" << prog_count << '\n';
        else if (prog_count == 0)
            std::cout << "[epg] WARNING: channel=" << ch.id
                      << " (" << ch.name << ") has 0 programmes in XMLTV window\n";
    }

    xml << "</tv>\n";
    return xml.str();
}

// ── M3U ──────────────────────────────────────────────────────────────────────

std::string EPGMaterializer::generateM3U(const std::string& base_url) {
    SQLite::Statement q(db_.get(),
        "SELECT channel_id, name, number FROM channel ORDER BY number");

    std::ostringstream m3u;
    m3u << "#EXTM3U\n";

    while (q.executeStep()) {
        std::string id   = q.getColumn(0).getString();
        std::string name = q.getColumn(1).getString();
        int         num  = q.getColumn(2).getInt();

        m3u << "#EXTINF:-1"
            << " tvg-id=\"kairos-"     << num  << "\""
            << " tvg-name=\""          << name << "\""
            << " tvg-logo=\"\""
            << " group-title=\"Kairos\""
            << " channel-number=\""    << num  << "\""
            << "," << name << "\n"
            << base_url << "/channels/" << id << "/stream\n";
    }
    return m3u.str();
}
