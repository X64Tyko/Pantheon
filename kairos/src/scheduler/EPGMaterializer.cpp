#include "EPGMaterializer.h"
#include "../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>
#include <sstream>

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
                                       std::time_t from, int horizon_hours) {
    std::time_t horizon = from + static_cast<std::time_t>(horizon_hours) * 3600;

    // Find the absolute last scheduled entry (any status) — its cursor_json is
    // the correct SimState to resume extension from.
    std::time_t last_end    = 0;
    std::string last_cursor = "{}";
    {
        SQLite::Statement q(db_.get(), R"(
            SELECT wall_clock_end, cursor_json FROM scheduled_program
            WHERE channel_id = ?
            ORDER BY wall_clock_end DESC LIMIT 1
        )");
        q.bind(1, channel_id);
        if (q.executeStep()) {
            last_end    = static_cast<std::time_t>(q.getColumn(0).getInt64());
            last_cursor = q.getColumn(1).getString();
        }
    }

    if (last_end >= horizon) return; // cache covers the requested range already

    // Project forward from the tail of the existing cache (or `from` if empty).
    std::time_t extend_from = (last_end > from) ? last_end : from;
    // Request slightly more than needed to avoid rounding gaps.
    int extend_hours = static_cast<int>((horizon - extend_from) / 3600) + 2;

    auto items = engine_.project(channel_id, extend_from, extend_hours, last_cursor);
    if (items.empty()) return;

    SQLite::Transaction txn(db_.get());
    SQLite::Statement ins(db_.get(), R"(
        INSERT OR IGNORE INTO scheduled_program
            (channel_id, block_id, item_type, item_id,
             wall_clock_start, wall_clock_end, cursor_json, created_at)
        VALUES (?,?,?,?,?,?,?,?)
    )");

    auto now = static_cast<int64_t>(std::time(nullptr));
    for (const auto& item : items) {
        std::time_t item_end = item.wall_clock_end_ms / 1000;
        if (item_end > horizon + 7200) break; // don't overshoot by more than 2 h

        ins.bind(1, channel_id);
        if (item.block_id.empty()) ins.bind(2); else ins.bind(2, item.block_id);
        ins.bind(3, item.item_type);
        ins.bind(4, item.item_id);
        ins.bind(5, item.wall_clock_start_ms / 1000);
        ins.bind(6, item_end);
        ins.bind(7, item.cursor_json);
        ins.bind(8, now);
        ins.exec();
        ins.reset();
    }
    txn.commit();
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
        // Join scheduled_program with episode/movie tables for metadata.
        SQLite::Statement q(db_.get(), R"(
            SELECT sp.item_type,
                   sp.wall_clock_start, sp.wall_clock_end,
                   COALESCE(e.title,  m.title,  '') AS item_title,
                   COALESCE(s.title,  '')            AS show_title,
                   COALESCE(e.season, 0)             AS season,
                   COALESCE(e.episode,0)             AS ep_num,
                   COALESCE(e.overview, m.overview, '') AS description
            FROM scheduled_program sp
            LEFT JOIN episode e ON sp.item_type = 'episode' AND sp.item_id = e.episode_id
            LEFT JOIN show    s ON sp.item_type = 'episode' AND e.show_id  = s.show_id
            LEFT JOIN movie   m ON sp.item_type = 'movie'   AND sp.item_id = m.movie_id
            WHERE sp.channel_id = ?
              AND sp.wall_clock_start >= ?
              AND sp.wall_clock_start <  ?
              AND sp.status != 'skipped'
            ORDER BY sp.wall_clock_start
        )");
        q.bind(1, ch.id);
        q.bind(2, static_cast<int64_t>(now));
        q.bind(3, static_cast<int64_t>(horizon));

        while (q.executeStep()) {
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
