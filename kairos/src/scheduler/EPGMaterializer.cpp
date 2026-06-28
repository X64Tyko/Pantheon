#include "EPGMaterializer.h"
#include "CursorState.h"
#include "Rng.h"
#include "../db/ChannelRepository.h"
#include "../db/CursorRepository.h"
#include "../db/Database.h"
#include "../db/ScheduleRepository.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_map>

#include "RuntimeFlags.h"

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

GenerateResult EPGMaterializer::generate(
    const std::string& channel_id, std::time_t from, int horizon_hours, int seed)
{
    using json = nlohmann::json;

    const std::time_t horizon     = from + static_cast<std::time_t>(horizon_hours) * 3600;
    const std::time_t from_days   = from / 86400;
    const std::time_t from_dow    = (from_days + 3) % 7;
    const std::time_t week_monday = (from_days - from_dow) * 86400;

    int init_seed = seed;
    if (init_seed < 0) {
        SQLite::Statement qs(db_.get(), "SELECT seed FROM channel WHERE channel_id=?");
        qs.bind(1, channel_id);
        if (qs.executeStep()) init_seed = qs.getColumn(0).getInt();
    }

    Xoshiro256  rng(init_seed >= 0 ? static_cast<uint64_t>(init_seed) : 0ULL);
    CursorState cs;
    GenerateResult result;

    bool has_anchor = false;
    {
        SQLite::Statement qa(db_.get(),
            "SELECT anchor_hashes FROM channel WHERE channel_id=?");
        qa.bind(1, channel_id);
        if (qa.executeStep() && !qa.getColumn(0).isNull()) {
            try {
                auto aj  = json::parse(qa.getColumn(0).getString());
                auto key = std::to_string(week_monday);
                if (aj.contains(key) && aj[key].is_object()) {
                    const auto& snap = aj[key];
                    has_anchor = true;
                    if (snap.contains("rng"))
                        rng = Xoshiro256::deserialize(snap["rng"].get<std::string>());
                    cs = CursorState::deserializeCursors(snap.dump());
                }
            } catch (...) {}
        }
    }

    // Bootstrap anchor captured in result.anchors for commit() to persist.
    if (!has_anchor) {
        result.anchors[week_monday] = json{
            {"rng",          rng.serialize()},
            {"cursors",      json::array()},
            {"block_states", json::array()}
        }.dump();
    }

    const int proj_hours = static_cast<int>((horizon - week_monday) / 3600) + 2;
    result.items = engine_.project(channel_id, week_monday, proj_hours, cs, rng,
                                   &result.anchors, &result.play_records);
    result.cursor_state = std::move(cs);

    // Detect divergences: new items that differ from what is currently committed.
    {
        std::unordered_map<std::time_t, std::pair<std::string, std::string>> existing;
        SQLite::Statement eq(db_.get(), R"(
            SELECT wall_clock_start, item_type, item_id
            FROM scheduled_program
            WHERE channel_id = ?
              AND wall_clock_end   > ?
              AND wall_clock_start < ?
        )");
        eq.bind(1, channel_id);
        eq.bind(2, static_cast<int64_t>(from));
        eq.bind(3, static_cast<int64_t>(horizon));
        while (eq.executeStep())
            existing[static_cast<std::time_t>(eq.getColumn(0).getInt64())] = {
                eq.getColumn(1).getString(), eq.getColumn(2).getString()
            };

        for (const auto& item : result.items) {
            std::time_t ws = item.wall_clock_start_ms / 1000;
            if (ws < from) continue;
            auto it = existing.find(ws);
            if (it != existing.end() &&
                (it->second.first != item.item_type || it->second.second != item.item_id))
            {
                result.divergences.push_back({
                    ws,
                    item.wall_clock_end_ms / 1000,
                    item.block_id,
                    it->second.first, it->second.second,
                    item.item_type,   item.item_id
                });
            }
        }
    }

    if (epgDebug())
        std::cout << "[epg] generate() channel=" << channel_id
                  << " items=" << result.items.size()
                  << " divergences=" << result.divergences.size() << '\n';

    return result;
}

void EPGMaterializer::commit(
    const std::string& channel_id, std::time_t horizon, GenerateResult& result)
{
    using json = nlohmann::json;
    auto now   = static_cast<int64_t>(std::time(nullptr));

    auto persistAnchors = [&]() {
        if (result.anchors.empty()) return;
        json existing = json::object();
        {
            SQLite::Statement qa(db_.get(),
                "SELECT anchor_hashes FROM channel WHERE channel_id=?");
            qa.bind(1, channel_id);
            if (qa.executeStep() && !qa.getColumn(0).isNull()) {
                try { existing = json::parse(qa.getColumn(0).getString()); } catch (...) {}
            }
        }
        for (auto& [ts, snap_str] : result.anchors) {
            try { existing[std::to_string(ts)] = json::parse(snap_str); } catch (...) {}
        }
        SQLite::Statement upd(db_.get(),
            "UPDATE channel SET anchor_hashes=? WHERE channel_id=?");
        upd.bind(1, existing.dump()); upd.bind(2, channel_id); upd.exec();
    };

    db_.get().exec("SAVEPOINT sp_commit");
    SQLite::Statement ins(db_.get(), R"(
        INSERT OR IGNORE INTO scheduled_program
            (channel_id, block_id, item_type, item_id,
             wall_clock_start, wall_clock_end, cursor_json, created_at, is_filler)
        VALUES (?,?,?,?,?,?,?,?,?)
    )");

    int inserted = 0, skipped = 0;
    for (const auto& item : result.items) {
        std::time_t item_end = item.wall_clock_end_ms / 1000;
        if (item_end > horizon + 7200) break;
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
        if (db_.get().getChanges() > 0) ++inserted; else ++skipped;
        ins.reset();
    }
    CursorRepository(db_).apply(channel_id, result.cursor_state);

    ScheduleRepository sched_repo(db_);
    for (const auto& r : result.play_records)
        sched_repo.recordScheduledPlayHistory(r.item_type, r.item_id, r.channel_id, r.block_id, r.aired_at);

    db_.get().exec("RELEASE SAVEPOINT sp_commit");
    persistAnchors();

    if (epgDebug())
        std::cout << "[epg] commit() channel=" << channel_id
                  << " inserted=" << inserted << " skipped=" << skipped << '\n';
}

void EPGMaterializer::ensureScheduled(const std::string& channel_id,
                                       std::time_t from, int horizon_hours,
                                       int seed) {
    // ── on_play mode: regenerate from current cursor position on every call. ──
    {
        bool on_play = ChannelRepository(db_).getAdvanceMode(channel_id) == "on_play";
        if (on_play) {
            auto now_ts = static_cast<int64_t>(std::time(nullptr));
            {
                SQLite::Statement d1(db_.get(),
                    "DELETE FROM scheduled_program WHERE channel_id=?");
                d1.bind(1, channel_id); d1.exec();
            }
            CursorState cs = CursorRepository(db_).load(channel_id);
            Xoshiro256  onplay_rng(seed >= 0 ? static_cast<uint64_t>(seed) : 0);
            auto items = engine_.project(channel_id, from, horizon_hours, cs, onplay_rng);

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
                ins.exec(); ins.reset();
            }
            db_.get().exec("RELEASE SAVEPOINT sp_ens");

            if (epgDebug())
                std::cout << "[epg] ensureScheduled on_play channel=" << channel_id
                          << " => " << items.size() << " items\n";
            return;
        }
    }

    // ── scheduled mode ───────────────────────────────────────────────────────
    const std::time_t horizon = from + static_cast<std::time_t>(horizon_hours) * 3600;

    if (epgDebug())
        std::cout << "[epg] ensureScheduled channel=" << channel_id
                  << " from=" << from << " horizon_hours=" << horizon_hours << '\n';

    auto result = generate(channel_id, from, horizon_hours, seed);
    if (result.items.empty()) {
        std::cout << "[epg] WARNING: generate() returned 0 items for channel="
                  << channel_id << " — EPG will be empty\n";
        return;
    }
    commit(channel_id, horizon, result);
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

    std::time_t now            = std::time(nullptr);
    std::time_t today_midnight = (now / 86400) * 86400;
    std::time_t horizon        = now + static_cast<std::time_t>(horizon_hours) * 3600;

    if (epgDebug())
        std::cout << "[epg] generateXMLTV: " << channels.size()
                  << " channel(s), horizon_hours=" << horizon_hours << '\n';

    // Extend cache from today midnight so programs that already aired today are
    // present in scheduled_program and appear in the XMLTV output.  Callers that
    // schedule from `now` leave a gap from midnight to the current moment.
    int hours_from_midnight = static_cast<int>((horizon - today_midnight) / 3600) + 1;
    for (const auto& ch : channels)
        ensureScheduled(ch.id, today_midnight, hours_from_midnight);

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
        q.bind(2, static_cast<int64_t>(today_midnight));
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
