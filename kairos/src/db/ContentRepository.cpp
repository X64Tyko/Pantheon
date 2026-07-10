#include "ContentRepository.h"
#include "Database.h"
#include "MetadataOverrideRepository.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {

// Mirrors ScraperManager's extractShowFolder season-name heuristic.
bool looksLikeSeasonFolder(const std::string& folderName) {
    std::string lower;
    lower.reserve(folderName.size());
    for (char c : folderName)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.starts_with("season") || lower.starts_with("specials")
        || lower.starts_with("extras") || lower.starts_with("bonus")
        || (lower.size() >= 2 && lower[0] == 's'
            && std::isdigit(static_cast<unsigned char>(lower[1])));
}

} // namespace

ContentRepository::ContentRepository(Database& db) : db_(db) {}

ContentRepository::RestrictionLookup ContentRepository::resolveForRestriction(
    const std::string& item_type, const std::string& item_id) {
    if (item_type == "episode") {
        SQLite::Statement q(db_.get(), R"(
            SELECT s.show_id, s.content_rating FROM episode e
            JOIN show s ON s.show_id = e.show_id
            WHERE e.episode_id = ?
        )");
        q.bind(1, item_id);
        if (q.executeStep())
            return {"show", q.getColumn(0).getString(), q.getColumn(1).getString()};
        return {"show", "", ""};
    }
    if (item_type == "movie") {
        SQLite::Statement q(db_.get(), "SELECT content_rating FROM movie WHERE movie_id = ?");
        q.bind(1, item_id);
        if (q.executeStep()) return {"movie", item_id, q.getColumn(0).getString()};
        return {"movie", item_id, ""};
    }
    if (item_type == "show") {
        SQLite::Statement q(db_.get(), "SELECT content_rating FROM show WHERE show_id = ?");
        q.bind(1, item_id);
        if (q.executeStep()) return {"show", item_id, q.getColumn(0).getString()};
        return {"show", item_id, ""};
    }
    return {item_type, item_id, ""};
}

// Override precedence rule (architecture.md "Metadata Override Layer"): a
// per-field metadata_override row always wins over whatever the base table
// holds, regardless of which source most recently synced over it.
namespace {
    void applyStr(const std::unordered_map<std::string, std::string>& ov,
                  const std::string& field, std::string& out) {
        auto it = ov.find(field);
        if (it != ov.end()) out = it->second;
    }
    void applyInt(const std::unordered_map<std::string, std::string>& ov,
                  const std::string& field, std::optional<int>& out) {
        auto it = ov.find(field);
        if (it != ov.end()) { try { out = std::stoi(it->second); } catch (...) {} }
    }
}

// ---------------------------------------------------------------------------
// Episode row helper
// ---------------------------------------------------------------------------

Episode ContentRepository::rowToEpisode(SQLite::Statement& q) {
    Episode e;
    e.episode_id  = q.getColumn(0).getString();
    e.show_id     = q.getColumn(1).getString();
    e.season      = q.getColumn(2).getInt();
    e.episode     = q.getColumn(3).getInt();
    e.title       = q.getColumn(4).getString();
    e.file_path   = q.getColumn(5).getString();
    e.duration_ms = q.getColumn(6).getInt64();
    e.overview    = q.getColumn(7).getString();
    e.air_date    = q.getColumn(8).getString();
    e.thumb       = q.getColumn(9).getString();
    return e;
}

// Extended variant that also reads tvdb_id, tmdb_id, imdb_id (columns 10–12).
Episode ContentRepository::rowToEpisodeFull(SQLite::Statement& q) {
    Episode e = rowToEpisode(q);
    e.tvdb_id = q.getColumn(10).getString();
    e.tmdb_id = q.getColumn(11).getString();
    e.imdb_id = q.getColumn(12).getString();
    return e;
}

// ---------------------------------------------------------------------------
// Episodes
// ---------------------------------------------------------------------------

std::vector<Episode> ContentRepository::getEpisodes(const std::string& show_id,
                                                      std::optional<int> season,
                                                      bool include_specials,
                                                      const std::string& episode_order) {
    std::string sql =
        "SELECT episode_id, show_id, season, episode, title, file_path, duration_ms,"
        " overview, air_date, thumb, tvdb_id, tmdb_id, imdb_id"
        // Linked specials (see ScraperManager's specials linking) are VOD-only
        // for now — excluded from linear scheduling, which is the only
        // caller of this function (listEpisodesForShow, the VOD path, is separate).
        " FROM episode WHERE show_id = ? AND linked_movie_id IS NULL";
    if (season)                         sql += " AND season = ?";
    if (!season && !include_specials)   sql += " AND season != 0";
    if (episode_order == "absolute")
        sql += " ORDER BY COALESCE(absolute_index, season * 10000 + episode)";
    else if (episode_order == "airdate")
        sql += " ORDER BY air_date, episode";
    else
        sql += " ORDER BY season, episode";

    SQLite::Statement q(db_.get(), sql);
    q.bind(1, show_id);
    if (season) q.bind(2, *season);

    std::vector<Episode> eps;
    while (q.executeStep()) eps.push_back(rowToEpisodeFull(q));
    return eps;
}

std::vector<Episode> ContentRepository::getPlayedEpisodes(const std::string& show_id,
                                                           const std::string& channel_id,
                                                           std::optional<int> season,
                                                           std::time_t before_time,
                                                           bool global_scope,
                                                           bool include_specials,
                                                           const std::string& episode_order) {
    std::string sql =
        "SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,"
        " e.duration_ms, e.overview, e.air_date, e.thumb, e.tvdb_id, e.tmdb_id, e.imdb_id"
        " FROM episode e WHERE e.show_id = ?";
    if (season)                       sql += " AND e.season = ?";
    if (!season && !include_specials) sql += " AND e.season != 0";
    // aired_at < before_time already excludes scheduled-but-not-yet-aired rows —
    // no separate is_scheduled filter needed. Most channels run in 'scheduled' mode,
    // where is_scheduled=1 rows (written by EPGMaterializer::commit()) are the only
    // play_history ever gets; requiring is_scheduled=0 here would make the rerun pool
    // never see them.
    // INDEXED BY forces the per-episode item_id lookup (idx_history_item):
    // without it, SQLite's planner picks idx_history_channel instead for this
    // correlated subquery and rescans the channel's *entire* play_history for
    // every candidate episode — measured ~130x slower on a channel with tens
    // of thousands of accumulated history rows (the common case for a
    // long-lived channel), which is what made schedule generation for
    // rerun/shuffle blocks pathologically slow.
    sql += " AND EXISTS (SELECT 1 FROM play_history ph INDEXED BY idx_history_item"
           " WHERE ph.item_type='episode' AND ph.item_id=e.episode_id";
    if (!global_scope) sql += " AND ph.channel_id=?";
    sql += " AND ph.aired_at < ?)";
    if (episode_order == "absolute")
        sql += " ORDER BY COALESCE(e.absolute_index, e.season * 10000 + e.episode)";
    else if (episode_order == "airdate")
        sql += " ORDER BY e.air_date, e.episode";
    else
        sql += " ORDER BY e.season, e.episode";

    SQLite::Statement q(db_.get(), sql);
    int idx = 1;
    q.bind(idx++, show_id);
    if (season)       q.bind(idx++, *season);
    if (!global_scope) q.bind(idx++, channel_id);
    q.bind(idx++, static_cast<int64_t>(before_time));

    std::vector<Episode> eps;
    while (q.executeStep()) eps.push_back(rowToEpisodeFull(q));
    return eps;
}

std::vector<Episode> ContentRepository::getPlayedEpisodesWithCooldown(
    const std::string& show_id,
    const std::string& channel_id,
    std::optional<int> season,
    int smart_pct,
    std::time_t before_time,
    bool global_scope,
    bool include_specials)
{
    const char* sql_ch_season = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph INDEXED BY idx_history_item ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.channel_id=? AND ph.aired_at < ?
        WHERE e.show_id=? AND e.season=?
        GROUP BY e.episode_id ORDER BY last_aired ASC
    )";
    const char* sql_ch_all = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph INDEXED BY idx_history_item ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.channel_id=? AND ph.aired_at < ?
        WHERE e.show_id=?
        GROUP BY e.episode_id ORDER BY last_aired ASC
    )";
    const char* sql_gl_season = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph INDEXED BY idx_history_item ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.aired_at < ?
        WHERE e.show_id=? AND e.season=?
        GROUP BY e.episode_id ORDER BY last_aired ASC
    )";
    const char* sql_gl_all = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph INDEXED BY idx_history_item ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.aired_at < ?
        WHERE e.show_id=?
        GROUP BY e.episode_id ORDER BY last_aired ASC
    )";

    SQLite::Statement q(db_.get(), global_scope
        ? (season ? sql_gl_season : sql_gl_all)
        : (season ? sql_ch_season : sql_ch_all));
    if (global_scope) {
        q.bind(1, static_cast<int64_t>(before_time)); q.bind(2, show_id);
        if (season) q.bind(3, *season);
    } else {
        q.bind(1, channel_id); q.bind(2, static_cast<int64_t>(before_time)); q.bind(3, show_id);
        if (season) q.bind(4, *season);
    }

    std::vector<Episode> all;
    while (q.executeStep()) {
        if (!include_specials && !season && q.getColumn(2).getInt() == 0) continue;
        all.push_back(rowToEpisode(q));
    }
    if (all.empty()) return all;

    int cooldown       = std::max(0, static_cast<int>(all.size()) * smart_pct / 100);
    int eligible_count = static_cast<int>(all.size()) - cooldown;
    if (eligible_count <= 0) return all;
    all.resize(static_cast<size_t>(eligible_count));
    return all;
}

// ---------------------------------------------------------------------------
// Movies
// ---------------------------------------------------------------------------

std::optional<Movie> ContentRepository::getMovie(const std::string& movie_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT movie_id, title, content_rating, file_path, duration_ms, year,
               overview, tagline, studio, director, genres, thumb, art, imdb_id, tmdb_id
        FROM movie WHERE movie_id = ?
    )");
    q.bind(1, movie_id);
    if (!q.executeStep()) return std::nullopt;

    Movie m;
    m.movie_id       = q.getColumn(0).getString();
    m.title          = q.getColumn(1).getString();
    m.content_rating = q.getColumn(2).getString();
    m.file_path      = q.getColumn(3).getString();
    m.duration_ms    = q.getColumn(4).getInt64();
    if (!q.getColumn(5).isNull()) m.year = q.getColumn(5).getInt();
    m.overview  = q.getColumn(6).getString();
    m.tagline   = q.getColumn(7).getString();
    m.studio    = q.getColumn(8).getString();
    m.director  = q.getColumn(9).getString();
    m.genres    = q.getColumn(10).getString();
    m.thumb     = q.getColumn(11).getString();
    m.art       = q.getColumn(12).getString();
    m.imdb_id   = q.getColumn(13).getString();
    m.tmdb_id   = q.getColumn(14).getString();
    return m;
}

// ---------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>>
ContentRepository::loadListItems(const std::string& content_type,
                                  const std::string& content_id) {
    const char* sql = (content_type == "filler_list")
        ? "SELECT item_type, item_id FROM filler_list_item WHERE filler_list_id=? ORDER BY position"
        : "SELECT item_type, item_id FROM playlist_item     WHERE playlist_id=?    ORDER BY position";
    SQLite::Statement q(db_.get(), sql);
    q.bind(1, content_id);
    std::vector<std::pair<std::string, std::string>> items;
    while (q.executeStep())
        items.emplace_back(q.getColumn(0).getString(), q.getColumn(1).getString());
    return items;
}

std::vector<FillerItem> ContentRepository::loadFillerItems(const std::string& content_type,
                                                            const std::string& content_id,
                                                            std::optional<int> season_filter) {
    std::vector<FillerItem> items;

    if (content_type == "filler_list") {
        SQLite::Statement q(db_.get(), R"(
            SELECT fi.item_type, fi.item_id,
                   COALESCE(e.duration_ms, m.duration_ms, 0)
            FROM filler_list_item fi
            LEFT JOIN episode e ON fi.item_type='episode' AND fi.item_id=e.episode_id
            LEFT JOIN movie   m ON fi.item_type='movie'   AND fi.item_id=m.movie_id
            WHERE fi.filler_list_id=? ORDER BY fi.position
        )");
        q.bind(1, content_id);
        while (q.executeStep())
            items.push_back({q.getColumn(0).getString(),
                             q.getColumn(1).getString(),
                             q.getColumn(2).getInt64()});
    } else if (content_type == "playlist") {
        SQLite::Statement q(db_.get(), R"(
            SELECT pi.item_type, pi.item_id,
                   COALESCE(e.duration_ms, m.duration_ms, 0)
            FROM playlist_item pi
            LEFT JOIN episode e ON pi.item_type='episode' AND pi.item_id=e.episode_id
            LEFT JOIN movie   m ON pi.item_type='movie'   AND pi.item_id=m.movie_id
            WHERE pi.playlist_id=? ORDER BY pi.position
        )");
        q.bind(1, content_id);
        while (q.executeStep())
            items.push_back({q.getColumn(0).getString(),
                             q.getColumn(1).getString(),
                             q.getColumn(2).getInt64()});
    } else if (content_type == "show") {
        std::string sql = "SELECT 'episode', episode_id, COALESCE(duration_ms, 0)"
                          " FROM episode WHERE show_id=?";
        if (season_filter.has_value()) sql += " AND season=?";
        sql += " ORDER BY season, episode";
        SQLite::Statement q(db_.get(), sql);
        q.bind(1, content_id);
        if (season_filter.has_value()) q.bind(2, season_filter.value());
        while (q.executeStep())
            items.push_back({q.getColumn(0).getString(),
                             q.getColumn(1).getString(),
                             q.getColumn(2).getInt64()});
    } else if (content_type == "movie") {
        SQLite::Statement q(db_.get(), "SELECT duration_ms FROM movie WHERE movie_id=?");
        q.bind(1, content_id);
        if (q.executeStep())
            items.push_back({"movie", content_id, q.getColumn(0).getInt64()});
    }

    return items;
}

// ---------------------------------------------------------------------------
// Playlist show_collection helpers
// ---------------------------------------------------------------------------

std::string ContentRepository::getPlaylistMode(const std::string& playlist_id) {
    SQLite::Statement q(db_.get(), "SELECT mode FROM playlist WHERE playlist_id=?");
    q.bind(1, playlist_id);
    if (q.executeStep()) return q.getColumn(0).getString();
    return "sequential";
}

std::vector<std::string> ContentRepository::getPlaylistShows(const std::string& playlist_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT e.show_id
        FROM playlist_item pi
        JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
        WHERE pi.playlist_id = ?
        GROUP BY e.show_id
        ORDER BY MIN(pi.position)
    )");
    q.bind(1, playlist_id);
    std::vector<std::string> shows;
    while (q.executeStep()) shows.push_back(q.getColumn(0).getString());
    return shows;
}

std::vector<Episode> ContentRepository::getPlaylistShowEpisodes(
    const std::string& playlist_id, const std::string& show_id)
{
    SQLite::Statement q(db_.get(), R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title,
               e.file_path, e.duration_ms, e.overview, e.air_date, e.thumb
        FROM playlist_item pi
        JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
        WHERE pi.playlist_id = ? AND e.show_id = ?
        ORDER BY pi.position
    )");
    q.bind(1, playlist_id); q.bind(2, show_id);
    std::vector<Episode> eps;
    while (q.executeStep()) eps.push_back(rowToEpisode(q));
    return eps;
}

int ContentRepository::getPlaylistItemCount(const std::string& playlist_id) {
    SQLite::Statement q(db_.get(),
        "SELECT COUNT(*) FROM playlist_item WHERE playlist_id=?");
    q.bind(1, playlist_id);
    if (q.executeStep()) return q.getColumn(0).getInt();
    return 0;
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

std::string ContentRepository::showTitle(const std::string& show_id) {
    try {
        SQLite::Statement q(db_.get(), "SELECT title FROM show WHERE show_id=?");
        q.bind(1, show_id);
        if (q.executeStep()) return q.getColumn(0).getString();
    } catch (...) {}
    return {};
}

// ---------------------------------------------------------------------------
// Episode group membership
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::pair<std::string, int>>
ContentRepository::getEpisodeGroupMap(const std::string& show_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT egm.episode_id, egm.group_id, egm.part_num
        FROM episode_group_member egm
        JOIN episode_group eg ON eg.group_id = egm.group_id
        WHERE eg.show_id = ?
    )");
    q.bind(1, show_id);
    std::unordered_map<std::string, std::pair<std::string, int>> result;
    while (q.executeStep())
        result[q.getColumn(0).getString()] = {q.getColumn(1).getString(),
                                               q.getColumn(2).getInt()};
    return result;
}

std::optional<std::string>
ContentRepository::findGroupPart1(const std::string& episode_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT egm2.episode_id
        FROM episode_group_member egm1
        JOIN episode_group_member egm2 ON egm2.group_id = egm1.group_id AND egm2.part_num = 1
        WHERE egm1.episode_id = ? AND egm1.part_num > 1
    )");
    q.bind(1, episode_id);
    if (!q.executeStep()) return std::nullopt;
    return q.getColumn(0).getString();
}

// ---------------------------------------------------------------------------
// Hot-ID queries (play-history recency for SmartShuffle cooldown)
// ---------------------------------------------------------------------------

std::unordered_set<std::string>
ContentRepository::getHotMovieIds(const std::string& channel_id,
                                   std::time_t before_time,
                                   int limit) {
    SQLite::Statement q(db_.get(), R"(
        SELECT item_id FROM (
            SELECT item_id, MAX(aired_at) AS last_aired
            FROM play_history
            WHERE item_type='movie' AND channel_id=? AND aired_at<?
            GROUP BY item_id
        ) ORDER BY last_aired DESC LIMIT ?
    )");
    q.bind(1, channel_id);
    q.bind(2, static_cast<int64_t>(before_time));
    q.bind(3, limit);
    std::unordered_set<std::string> ids;
    while (q.executeStep()) ids.insert(q.getColumn(0).getString());
    return ids;
}

std::unordered_set<std::string>
ContentRepository::getHotEpisodeIds(const std::string& channel_id,
                                     std::time_t before_time,
                                     const std::string& show_id,
                                     int limit) {
    SQLite::Statement q(db_.get(), R"(
        SELECT item_id FROM (
            SELECT ph.item_id, MAX(ph.aired_at) AS last_aired
            FROM play_history ph
            JOIN episode e ON e.episode_id = ph.item_id
            WHERE ph.item_type='episode' AND ph.channel_id=? AND ph.aired_at<? AND e.show_id=?
            GROUP BY ph.item_id
        ) ORDER BY last_aired DESC LIMIT ?
    )");
    q.bind(1, channel_id);
    q.bind(2, static_cast<int64_t>(before_time));
    q.bind(3, show_id);
    q.bind(4, limit);
    std::unordered_set<std::string> ids;
    while (q.executeStep()) ids.insert(q.getColumn(0).getString());
    return ids;
}

std::unordered_map<std::string, int64_t>
ContentRepository::getLastPlayedMap(const std::string& channel_id,
                                     std::time_t before_time) {
    // filler_play_history, not play_history: filler picks are tracked separately so
    // they never pollute getHotMovieIds/getHotEpisodeIds (content-side cooldown).
    const char* sql = (before_time > 0)
        ? "SELECT item_id, MAX(aired_at) FROM filler_play_history WHERE channel_id=? AND aired_at<=? GROUP BY item_id"
        : "SELECT item_id, MAX(aired_at) FROM filler_play_history WHERE channel_id=? GROUP BY item_id";
    SQLite::Statement q(db_.get(), sql);
    q.bind(1, channel_id);
    if (before_time > 0) q.bind(2, static_cast<int64_t>(before_time));
    std::unordered_map<std::string, int64_t> result;
    while (q.executeStep())
        result[q.getColumn(0).getString()] = q.getColumn(1).getInt64();
    return result;
}

// ---------------------------------------------------------------------------
// SQL fragment helpers (file-scope, not exposed in header)
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> splitSemi(const std::string& s) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ';')) {
        auto b = tok.find_first_not_of(" \t");
        auto e = tok.find_last_not_of(" \t");
        if (b != std::string::npos) parts.push_back(tok.substr(b, e - b + 1));
    }
    return parts;
}

void appendIn(const std::string& col, const std::string& raw,
              std::string& extras, std::vector<std::string>& vals) {
    auto parts = splitSemi(raw);
    if (parts.empty()) return;
    if (parts.size() == 1) {
        extras += " AND " + col + " = ?";
    } else {
        std::string ph;
        for (size_t i = 0; i < parts.size(); ++i) ph += (i ? ",?" : "?");
        extras += " AND " + col + " IN (" + ph + ")";
    }
    for (auto& p : parts) vals.push_back(p);
}

void appendJsonIn(const std::string& tbl, const std::string& col, const std::string& raw,
                  std::string& extras, std::vector<std::string>& vals) {
    auto parts = splitSemi(raw);
    if (parts.empty()) return;
    if (parts.size() == 1) {
        extras += " AND EXISTS (SELECT 1 FROM json_each(NULLIF(" + tbl + "." + col + ",''))"
                  " WHERE json_each.value = ?)";
    } else {
        std::string ph;
        for (size_t i = 0; i < parts.size(); ++i) ph += (i ? ",?" : "?");
        extras += " AND EXISTS (SELECT 1 FROM json_each(NULLIF(" + tbl + "." + col + ",''))"
                  " WHERE json_each.value IN (" + ph + "))";
    }
    for (auto& p : parts) vals.push_back(p);
}

// Appends a restriction WHERE clause (override-wins-over-ceiling, mirrors
// RestrictionRepository::isAllowed) to the same extras/extra_vals idiom used
// above by the other dynamic filters. entity_type is "show" or "movie";
// id_col/rating_col are the already-aliased column references in the query
// being built (e.g. "s.show_id"/"s.content_rating"). Ceiling comparison is
// done as an explicit CAST(...AS INTEGER), matching this file's existing
// idiom for binding numeric values through the string-based extra_vals
// vector (see the `year` filter above) rather than relying on SQLite's
// storage-class comparison rules, which sort TEXT after INTEGER regardless
// of numeric content.
void appendRestriction(const RestrictionContext& ctx, const std::string& entity_type,
                       const std::string& id_col, const std::string& rating_col,
                       std::string& extras, std::vector<std::string>& vals) {
    if (!ctx.restricted) return;
    const std::string case_expr = (entity_type == "show")
        ? "CASE UPPER(IFNULL(" + rating_col + ",'')) "
          "WHEN 'TV-Y' THEN 0 WHEN 'TV-Y7' THEN 1 WHEN 'TV-G' THEN 2 "
          "WHEN 'TV-PG' THEN 3 WHEN 'TV-14' THEN 4 WHEN 'TV-MA' THEN 5 ELSE 6 END"
        : "CASE UPPER(IFNULL(" + rating_col + ",'')) "
          "WHEN 'G' THEN 0 WHEN 'PG' THEN 1 WHEN 'PG-13' THEN 2 "
          "WHEN 'R' THEN 3 WHEN 'NC-17' THEN 4 ELSE 5 END";
    extras += " AND ("
              "  EXISTS (SELECT 1 FROM user_content_override uco WHERE uco.user_id=? "
              "          AND uco.entity_type=? AND uco.entity_id=" + id_col + " AND uco.mode='allow')"
              "  OR ("
              "    NOT EXISTS (SELECT 1 FROM user_content_override uco2 WHERE uco2.user_id=? "
              "                AND uco2.entity_type=? AND uco2.entity_id=" + id_col + " AND uco2.mode='block')"
              "    AND " + case_expr + " <= CAST(? AS INTEGER)"
              "  )"
              ")";
    vals.push_back(ctx.user_id); vals.push_back(entity_type);
    vals.push_back(ctx.user_id); vals.push_back(entity_type);
    vals.push_back(std::to_string(ctx.rating_ceiling));
}

} // namespace

// ---------------------------------------------------------------------------
// API-layer queries
// ---------------------------------------------------------------------------

std::vector<LibraryRow> ContentRepository::listLibraries() {
    SQLite::Statement q(db_.get(), R"(
        SELECT ml.library_id, ml.source_id, ml.display_name, ml.library_type,
               ms.display_name AS source_name, ms.source_type, ml.show_on_home
        FROM media_library ml
        JOIN media_source ms ON ms.source_id = ml.source_id
        ORDER BY ms.display_name, ml.display_name
    )");
    std::vector<LibraryRow> rows;
    while (q.executeStep()) {
        LibraryRow r;
        r.library_id   = q.getColumn(0).getString();
        r.source_id    = q.getColumn(1).getString();
        r.display_name = q.getColumn(2).getString();
        r.library_type = q.getColumn(3).getString();
        r.source_name  = q.getColumn(4).getString();
        r.source_type  = q.getColumn(5).getString();
        r.show_on_home = q.getColumn(6).getInt() != 0;
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<std::string> ContentRepository::getMetadataValues(const std::string& field,
                                                               const std::string& type,
                                                               const std::string& library_id) {
    std::set<std::string> seen;
    std::vector<std::string> values;

    std::string show_lib  = library_id.empty() ? "" :
        " AND EXISTS (SELECT 1 FROM source_mapping sm"
        " WHERE sm.kairos_id = s.show_id AND sm.item_type = 'show'"
        " AND sm.library_id = ?)";
    std::string movie_lib = library_id.empty() ? "" :
        " AND EXISTS (SELECT 1 FROM source_mapping sm"
        " WHERE sm.kairos_id = m.movie_id AND sm.item_type = 'movie'"
        " AND sm.library_id = ?)";

    std::vector<std::string> lib;
    if (!library_id.empty()) lib.push_back(library_id);

    auto collect = [&](const std::string& sql, const std::vector<std::string>& binds) {
        SQLite::Statement q(db_.get(), sql);
        for (int i = 0; i < (int)binds.size(); ++i) q.bind(i + 1, binds[i]);
        while (q.executeStep()) {
            auto v = q.getColumn(0).getString();
            if (!v.empty() && seen.insert(v).second) values.push_back(v);
        }
    };

    if (field == "genre") {
        if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(NULLIF(s.genres,'')) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
        if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(NULLIF(m.genres,'')) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
    } else if (field == "studio") {
        if (type != "movie") collect("SELECT DISTINCT studio FROM show s WHERE studio != ''"  + show_lib  + " ORDER BY studio", lib);
        if (type != "show")  collect("SELECT DISTINCT studio FROM movie m WHERE studio != ''" + movie_lib + " ORDER BY studio", lib);
    } else if (field == "director") {
        if (type != "show")  collect("SELECT DISTINCT director FROM movie m WHERE director != ''" + movie_lib + " ORDER BY director", lib);
    } else if (field == "content_rating") {
        if (type != "movie") collect("SELECT DISTINCT content_rating FROM show s WHERE content_rating != ''"  + show_lib  + " ORDER BY content_rating", lib);
        if (type != "show")  collect("SELECT DISTINCT content_rating FROM movie m WHERE content_rating != ''" + movie_lib + " ORDER BY content_rating", lib);
    } else if (field == "label") {
        if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(NULLIF(s.labels,'')) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
        if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(NULLIF(m.labels,'')) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
    } else if (field == "network") {
        if (type != "movie") collect("SELECT DISTINCT network FROM show s WHERE network != ''" + show_lib + " ORDER BY network", lib);
    } else if (field == "actor") {
        if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(NULLIF(s.actors,'')) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
        if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(NULLIF(m.actors,'')) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
    } else if (field == "country") {
        if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(NULLIF(s.countries,'')) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
        if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(NULLIF(m.countries,'')) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
    } else if (field == "collection") {
        if (type != "movie") collect("SELECT DISTINCT je.value FROM show s, json_each(NULLIF(s.collections,'')) je WHERE je.value != ''" + show_lib  + " ORDER BY je.value", lib);
        if (type != "show")  collect("SELECT DISTINCT je.value FROM movie m, json_each(NULLIF(m.collections,'')) je WHERE je.value != ''" + movie_lib + " ORDER BY je.value", lib);
    }

    std::sort(values.begin(), values.end());
    return values;
}

ShowListResult ContentRepository::searchShows(const ShowSearchParams& p) {
    std::string extras;
    std::vector<std::string> extra_vals;
    if (!p.q.empty()) {
        extras += " AND (s.title LIKE '%'||?||'%' OR s.network LIKE '%'||?||'%' OR s.studio LIKE '%'||?||'%'"
                  " OR EXISTS (SELECT 1 FROM json_each(NULLIF(s.labels,'')) je WHERE je.value LIKE '%'||?||'%')"
                  " OR EXISTS (SELECT 1 FROM json_each(NULLIF(s.genres,'')) je WHERE je.value LIKE '%'||?||'%')"
                  " OR EXISTS (SELECT 1 FROM json_each(NULLIF(s.actors,'')) je WHERE je.value LIKE '%'||?||'%'))";
        for (int i = 0; i < 6; ++i) extra_vals.push_back(p.q);
    }
    if (!p.genre.empty())        appendJsonIn("s", "genres",      p.genre,      extras, extra_vals);
    if (!p.year.empty())         { extras += " AND s.year = CAST(? AS INTEGER)"; extra_vals.push_back(p.year); }
    if (!p.content_rating.empty()) appendIn("s.content_rating",   p.content_rating, extras, extra_vals);
    if (!p.label.empty())        appendJsonIn("s", "labels",      p.label,      extras, extra_vals);
    if (!p.network.empty())      { extras += " AND s.network LIKE '%' || ? || '%'"; extra_vals.push_back(p.network); }
    if (!p.actor.empty())        appendJsonIn("s", "actors",      p.actor,      extras, extra_vals);
    if (!p.country.empty())      appendJsonIn("s", "countries",   p.country,    extras, extra_vals);
    if (!p.collection.empty())   appendJsonIn("s", "collections", p.collection, extras, extra_vals);
    if (!p.studio.empty())       { extras += " AND s.studio LIKE '%' || ? || '%'"; extra_vals.push_back(p.studio); }
    appendRestriction(p.restriction, "show", "s.show_id", "s.content_rating", extras, extra_vals);

    auto bindExtras = [&](SQLite::Statement& q, int& idx) {
        for (const auto& v : extra_vals) q.bind(idx++, v);
    };

    const std::string src_subq = R"(
        COALESCE((SELECT ms.base_url FROM source_mapping sm2
                  JOIN media_source ms ON ms.source_id = sm2.source_id
                  WHERE sm2.kairos_id = s.show_id AND sm2.item_type = 'show'
                  LIMIT 1), '') AS source_base_url,
        COALESCE((SELECT sm3.library_id FROM source_mapping sm3
                  WHERE sm3.kairos_id = s.show_id AND sm3.item_type = 'show'
                        AND sm3.library_id IS NOT NULL
                  LIMIT 1), '') AS library_id)";
    const std::string order_clause =
        (p.sort == "recently_added") ? " ORDER BY s.rowid DESC" :
        (p.sort == "recently_aired") ? R"( ORDER BY (
                                             SELECT MAX(e2.air_date) FROM episode e2
                                             WHERE e2.show_id = s.show_id
                                               AND e2.air_date != '' AND e2.air_date <= date('now')
                                           ) DESC)" :
        (p.sort == "random")         ? " ORDER BY RANDOM()" :
                                       " ORDER BY s.title";
    const std::string show_select = R"(
            SELECT s.show_id, s.title, s.content_rating,
                   COUNT(DISTINCT e.episode_id) AS episode_count, s.year,
                   s.thumb, s.art, s.audience_rating, s.match_status, s.match_score, )" + src_subq;

    auto parseShowRow = [](SQLite::Statement& q) {
        ShowRow r;
        r.show_id         = q.getColumn(0).getString();
        r.title           = q.getColumn(1).getString();
        r.content_rating  = q.getColumn(2).getString();
        r.episode_count   = q.getColumn(3).getInt();
        if (!q.getColumn(4).isNull()) r.year             = q.getColumn(4).getInt();
        r.thumb           = q.getColumn(5).getString();
        r.art             = q.getColumn(6).getString();
        if (!q.getColumn(7).isNull()) r.audience_rating  = q.getColumn(7).getDouble();
        r.match_status    = q.getColumn(8).getString();
        if (!q.getColumn(9).isNull()) r.match_score      = q.getColumn(9).getDouble();
        r.source_base_url = q.getColumn(10).getString();
        r.library_id      = q.getColumn(11).getString();
        return r;
    };

    // Only applied when browsing unscoped (no explicit library_id) — a
    // filler/bumper library opted out of Home stays fully visible when
    // browsed directly, e.g. via the Library page's own switcher.
    const std::string home_exclude = R"(
        AND NOT EXISTS (
            SELECT 1 FROM source_mapping sm4
            JOIN media_library ml4 ON ml4.library_id = sm4.library_id
            WHERE sm4.kairos_id = s.show_id AND sm4.item_type = 'show' AND ml4.show_on_home = 0
        ))";

    ShowListResult result;
    if (p.library_id.empty()) {
        SQLite::Statement cnt(db_.get(), "SELECT COUNT(*) FROM show s WHERE 1=1" + extras + home_exclude);
        int idx = 1; bindExtras(cnt, idx);
        if (cnt.executeStep()) result.total = cnt.getColumn(0).getInt();

        SQLite::Statement q(db_.get(), show_select +
            R"( FROM show s LEFT JOIN episode e ON e.show_id = s.show_id
            WHERE 1=1)" + extras + home_exclude + R"( GROUP BY s.show_id)" + order_clause + " LIMIT ? OFFSET ?");
        idx = 1; bindExtras(q, idx);
        q.bind(idx++, p.limit); q.bind(idx++, p.offset);
        while (q.executeStep()) result.items.push_back(parseShowRow(q));
    } else {
        SQLite::Statement cnt(db_.get(), R"(
            SELECT COUNT(DISTINCT s.show_id) FROM show s
            JOIN source_mapping sm ON sm.kairos_id = s.show_id
                AND sm.item_type = 'show' AND sm.library_id = ?
            WHERE 1=1)" + extras);
        int idx = 1; cnt.bind(idx++, p.library_id); bindExtras(cnt, idx);
        if (cnt.executeStep()) result.total = cnt.getColumn(0).getInt();

        SQLite::Statement q(db_.get(), show_select +
            R"( FROM show s
            JOIN source_mapping sm ON sm.kairos_id = s.show_id
                AND sm.item_type = 'show' AND sm.library_id = ?
            LEFT JOIN episode e ON e.show_id = s.show_id
            WHERE 1=1)" + extras + R"( GROUP BY s.show_id)" + order_clause + " LIMIT ? OFFSET ?");
        idx = 1; q.bind(idx++, p.library_id); bindExtras(q, idx);
        q.bind(idx++, p.limit); q.bind(idx++, p.offset);
        while (q.executeStep()) result.items.push_back(parseShowRow(q));
    }
    return result;
}

MovieListResult ContentRepository::searchMovies(const MovieSearchParams& p) {
    std::string extras;
    std::vector<std::string> extra_vals;
    if (!p.q.empty()) {
        extras += " AND (m.title LIKE '%'||?||'%' OR m.studio LIKE '%'||?||'%'"
                  " OR EXISTS (SELECT 1 FROM json_each(NULLIF(m.labels,'')) je WHERE je.value LIKE '%'||?||'%')"
                  " OR EXISTS (SELECT 1 FROM json_each(NULLIF(m.genres,'')) je WHERE je.value LIKE '%'||?||'%')"
                  " OR EXISTS (SELECT 1 FROM json_each(NULLIF(m.actors,'')) je WHERE je.value LIKE '%'||?||'%'))";
        for (int i = 0; i < 5; ++i) extra_vals.push_back(p.q);
    }
    if (!p.genre.empty())          appendJsonIn("m", "genres",      p.genre,         extras, extra_vals);
    if (!p.year.empty())           { extras += " AND m.year = CAST(? AS INTEGER)"; extra_vals.push_back(p.year); }
    if (!p.content_rating.empty()) appendIn("m.content_rating",     p.content_rating, extras, extra_vals);
    if (!p.label.empty())          appendJsonIn("m", "labels",      p.label,         extras, extra_vals);
    if (!p.actor.empty())          appendJsonIn("m", "actors",      p.actor,         extras, extra_vals);
    if (!p.country.empty())        appendJsonIn("m", "countries",   p.country,       extras, extra_vals);
    if (!p.collection.empty())     appendJsonIn("m", "collections", p.collection,    extras, extra_vals);
    if (!p.studio.empty())         { extras += " AND m.studio LIKE '%' || ? || '%'"; extra_vals.push_back(p.studio); }
    // recently_released is meaningless for movies the source never gave a
    // release_date (most commonly AniDB, which doesn't fetch one at all) —
    // exclude them rather than let them sort to one end of the list.
    if (p.sort == "recently_released") extras += " AND m.release_date != ''";
    appendRestriction(p.restriction, "movie", "m.movie_id", "m.content_rating", extras, extra_vals);

    auto bindExtras = [&](SQLite::Statement& q, int& idx) {
        for (const auto& v : extra_vals) q.bind(idx++, v);
    };

    const std::string msrc_subq = R"(
        COALESCE((SELECT ms.base_url FROM source_mapping sm2
                  JOIN media_source ms ON ms.source_id = sm2.source_id
                  WHERE sm2.kairos_id = m.movie_id AND sm2.item_type = 'movie'
                  LIMIT 1), '') AS source_base_url,
        COALESCE((SELECT sm3.library_id FROM source_mapping sm3
                  WHERE sm3.kairos_id = m.movie_id AND sm3.item_type = 'movie'
                        AND sm3.library_id IS NOT NULL
                  LIMIT 1), '') AS library_id)";
    const std::string morder =
        (p.sort == "recently_added")    ? " ORDER BY m.rowid DESC" :
        (p.sort == "recently_released") ? " ORDER BY m.release_date DESC" :
        (p.sort == "random")            ? " ORDER BY RANDOM()" :
                                          " ORDER BY m.title";
    const std::string movie_select =
        "SELECT m.movie_id, m.title, m.content_rating, m.duration_ms, m.year, m.release_date,"
        " m.thumb, m.art, m.audience_rating, m.match_status, m.match_score," + msrc_subq;

    auto parseMovieRow = [](SQLite::Statement& q) {
        MovieRow r;
        r.movie_id        = q.getColumn(0).getString();
        r.title           = q.getColumn(1).getString();
        r.content_rating  = q.getColumn(2).getString();
        r.duration_ms     = q.getColumn(3).getInt64();
        if (!q.getColumn(4).isNull()) r.year             = q.getColumn(4).getInt();
        r.release_date    = q.getColumn(5).getString();
        r.thumb           = q.getColumn(6).getString();
        r.art             = q.getColumn(7).getString();
        if (!q.getColumn(8).isNull()) r.audience_rating  = q.getColumn(8).getDouble();
        r.match_status    = q.getColumn(9).getString();
        if (!q.getColumn(10).isNull()) r.match_score     = q.getColumn(10).getDouble();
        r.source_base_url = q.getColumn(11).getString();
        r.library_id      = q.getColumn(12).getString();
        return r;
    };

    // See searchShows()'s identical home_exclude — same reasoning.
    const std::string home_exclude = R"(
        AND NOT EXISTS (
            SELECT 1 FROM source_mapping sm4
            JOIN media_library ml4 ON ml4.library_id = sm4.library_id
            WHERE sm4.kairos_id = m.movie_id AND sm4.item_type = 'movie' AND ml4.show_on_home = 0
        ))";

    MovieListResult result;
    if (p.library_id.empty()) {
        SQLite::Statement cnt(db_.get(), "SELECT COUNT(*) FROM movie m WHERE 1=1" + extras + home_exclude);
        int idx = 1; bindExtras(cnt, idx);
        if (cnt.executeStep()) result.total = cnt.getColumn(0).getInt();

        SQLite::Statement q(db_.get(),
            movie_select + " FROM movie m WHERE 1=1" + extras + home_exclude + morder + " LIMIT ? OFFSET ?");
        idx = 1; bindExtras(q, idx);
        q.bind(idx++, p.limit); q.bind(idx++, p.offset);
        while (q.executeStep()) result.items.push_back(parseMovieRow(q));
    } else {
        SQLite::Statement cnt(db_.get(), R"(
            SELECT COUNT(DISTINCT m.movie_id) FROM movie m
            JOIN source_mapping sm ON sm.kairos_id = m.movie_id
                AND sm.item_type = 'movie' AND sm.library_id = ?
            WHERE 1=1)" + extras);
        int idx = 1; cnt.bind(idx++, p.library_id); bindExtras(cnt, idx);
        if (cnt.executeStep()) result.total = cnt.getColumn(0).getInt();

        SQLite::Statement q(db_.get(),
            movie_select + R"( FROM movie m
            JOIN source_mapping sm ON sm.kairos_id = m.movie_id
                AND sm.item_type = 'movie' AND sm.library_id = ?
            WHERE 1=1)" + extras + morder + " LIMIT ? OFFSET ?");
        idx = 1; q.bind(idx++, p.library_id); bindExtras(q, idx);
        q.bind(idx++, p.limit); q.bind(idx++, p.offset);
        while (q.executeStep()) result.items.push_back(parseMovieRow(q));
    }
    return result;
}

std::optional<ShowDetail> ContentRepository::getShowDetail(const std::string& show_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT s.show_id, s.title, s.content_rating, s.overview, s.studio, s.status,
               s.genres, s.thumb, s.art, s.imdb_id, s.tvdb_id, s.tmdb_id,
               s.originally_available_at, s.year, s.audience_rating, s.locked,
               COUNT(e.episode_id) AS episode_count,
               s.labels, s.network, s.actors, s.countries, s.collections,
               s.match_status, s.match_score, s.match_confirmed, s.skip_scraping,
               s.find_specials, s.episode_display_order
        FROM show s
        LEFT JOIN episode e ON e.show_id = s.show_id
        WHERE s.show_id = ?
        GROUP BY s.show_id
    )");
    q.bind(1, show_id);
    if (!q.executeStep()) return std::nullopt;

    ShowDetail d;
    d.show_id                  = q.getColumn(0).getString();
    d.title                    = q.getColumn(1).getString();
    d.content_rating           = q.getColumn(2).getString();
    d.overview                 = q.getColumn(3).getString();
    d.studio                   = q.getColumn(4).getString();
    d.status                   = q.getColumn(5).getString();
    d.genres                   = q.getColumn(6).getString();
    d.thumb                    = q.getColumn(7).getString();
    d.art                      = q.getColumn(8).getString();
    d.imdb_id                  = q.getColumn(9).getString();
    d.tvdb_id                  = q.getColumn(10).getString();
    d.tmdb_id                  = q.getColumn(11).getString();
    d.originally_available_at  = q.getColumn(12).getString();
    if (!q.getColumn(13).isNull()) d.year             = q.getColumn(13).getInt();
    if (!q.getColumn(14).isNull()) d.audience_rating  = q.getColumn(14).getDouble();
    d.locked        = q.getColumn(15).getInt() != 0;
    d.episode_count = q.getColumn(16).getInt();
    d.labels        = q.getColumn(17).getString();
    d.network       = q.getColumn(18).getString();
    d.actors        = q.getColumn(19).getString();
    d.countries     = q.getColumn(20).getString();
    d.collections   = q.getColumn(21).getString();
    d.match_status  = q.getColumn(22).getString();
    if (!q.getColumn(23).isNull()) d.match_score = q.getColumn(23).getDouble();
    d.match_confirmed = q.getColumn(24).getInt() != 0;
    d.skip_scraping   = q.getColumn(25).getInt() != 0;
    d.find_specials   = q.getColumn(26).getInt() != 0;
    d.episode_display_order = q.getColumn(27).getString();

    {
        SQLite::Statement sm(db_.get(), R"(
            SELECT sm.external_id, sm.source_id, ms.base_url
            FROM source_mapping sm
            JOIN media_source ms ON ms.source_id = sm.source_id
            WHERE sm.item_type = 'show' AND sm.kairos_id = ?
            LIMIT 1
        )");
        sm.bind(1, show_id);
        if (sm.executeStep()) {
            d.external_id     = sm.getColumn(0).getString();
            d.source_id       = sm.getColumn(1).getString();
            d.source_base_url = sm.getColumn(2).getString();
        }
    }

    {
        SQLite::Statement sq(db_.get(),
            "SELECT DISTINCT e.season, COALESCE(ss.season_name, '') "
            "FROM episode e "
            "LEFT JOIN show_season ss ON ss.show_id = e.show_id AND ss.season = e.season "
            "WHERE e.show_id = ? ORDER BY e.season");
        sq.bind(1, show_id);
        while (sq.executeStep())
            d.seasons.push_back({sq.getColumn(0).getInt(), sq.getColumn(1).getString()});
    }

    {
        auto ov = MetadataOverrideRepository(db_).getAll("show", show_id);
        applyStr(ov, "title",                   d.title);
        applyStr(ov, "overview",                d.overview);
        applyStr(ov, "studio",                  d.studio);
        applyStr(ov, "status",                  d.status);
        applyStr(ov, "content_rating",          d.content_rating);
        applyStr(ov, "originally_available_at", d.originally_available_at);
        applyStr(ov, "imdb_id",                 d.imdb_id);
        applyStr(ov, "tvdb_id",                 d.tvdb_id);
        applyStr(ov, "tmdb_id",                 d.tmdb_id);
        applyStr(ov, "thumb",                   d.thumb);
        applyStr(ov, "art",                     d.art);
        applyStr(ov, "genres",                  d.genres);
        applyStr(ov, "labels",                  d.labels);
        applyStr(ov, "network",                 d.network);
        applyStr(ov, "actors",                  d.actors);
        applyStr(ov, "countries",               d.countries);
        applyStr(ov, "collections",             d.collections);
        applyInt(ov, "year",                    d.year);
    }

    d.folder_path = getShowFolderPath(show_id).value_or("");

    return d;
}

std::string ContentRepository::parentDir(const std::string& file_path) {
    auto slash = file_path.find_last_of('/');
    return slash == std::string::npos ? std::string() : file_path.substr(0, slash);
}

std::optional<std::string> ContentRepository::getShowFolderPath(const std::string& show_id) {
    // One episode's folder is enough: step up past a season subfolder if present,
    // otherwise that folder is already the show root.
    SQLite::Statement q(db_.get(),
        "SELECT file_path FROM episode WHERE show_id = ? AND file_path != '' LIMIT 1");
    q.bind(1, show_id);
    if (!q.executeStep()) return std::nullopt;

    std::string dir = parentDir(q.getColumn(0).getString());
    if (dir.empty()) return std::nullopt;

    std::string name = dir.substr(dir.find_last_of('/') + 1);
    if (looksLikeSeasonFolder(name)) dir = parentDir(dir);

    return dir.empty() ? std::nullopt : std::make_optional(dir);
}

std::optional<MovieDetail> ContentRepository::getMovieDetail(const std::string& movie_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT movie_id, title, content_rating, duration_ms, year,
               overview, tagline, studio, director, genres, thumb, art,
               imdb_id, tmdb_id, audience_rating, locked,
               labels, actors, countries, collections, release_date,
               file_path, match_status, match_score, match_confirmed, skip_scraping
        FROM movie WHERE movie_id = ?
    )");
    q.bind(1, movie_id);
    if (!q.executeStep()) return std::nullopt;

    MovieDetail d;
    d.movie_id       = q.getColumn(0).getString();
    d.title          = q.getColumn(1).getString();
    d.content_rating = q.getColumn(2).getString();
    d.duration_ms    = q.getColumn(3).getInt64();
    if (!q.getColumn(4).isNull()) d.year = q.getColumn(4).getInt();
    d.overview       = q.getColumn(5).getString();
    d.tagline        = q.getColumn(6).getString();
    d.studio         = q.getColumn(7).getString();
    d.director       = q.getColumn(8).getString();
    d.genres         = q.getColumn(9).getString();
    d.thumb          = q.getColumn(10).getString();
    d.art            = q.getColumn(11).getString();
    d.imdb_id        = q.getColumn(12).getString();
    d.tmdb_id        = q.getColumn(13).getString();
    if (!q.getColumn(14).isNull()) d.audience_rating = q.getColumn(14).getDouble();
    d.locked         = q.getColumn(15).getInt() != 0;
    d.labels         = q.getColumn(16).getString();
    d.actors         = q.getColumn(17).getString();
    d.countries      = q.getColumn(18).getString();
    d.collections    = q.getColumn(19).getString();
    d.release_date   = q.getColumn(20).getString();
    d.file_path      = q.getColumn(21).getString();
    d.folder_path    = parentDir(d.file_path);
    d.match_status   = q.getColumn(22).getString();
    if (!q.getColumn(23).isNull()) d.match_score = q.getColumn(23).getDouble();
    d.match_confirmed = q.getColumn(24).getInt() != 0;
    d.skip_scraping   = q.getColumn(25).getInt() != 0;

    SQLite::Statement sm(db_.get(), R"(
        SELECT sm.external_id, sm.source_id, ms.base_url
        FROM source_mapping sm
        JOIN media_source ms ON ms.source_id = sm.source_id
        WHERE sm.item_type = 'movie' AND sm.kairos_id = ?
        LIMIT 1
    )");
    sm.bind(1, movie_id);
    if (sm.executeStep()) {
        d.external_id     = sm.getColumn(0).getString();
        d.source_id       = sm.getColumn(1).getString();
        d.source_base_url = sm.getColumn(2).getString();
    }

    {
        auto ov = MetadataOverrideRepository(db_).getAll("movie", movie_id);
        applyStr(ov, "title",          d.title);
        applyStr(ov, "overview",       d.overview);
        applyStr(ov, "tagline",        d.tagline);
        applyStr(ov, "studio",         d.studio);
        applyStr(ov, "director",       d.director);
        applyStr(ov, "content_rating", d.content_rating);
        applyStr(ov, "imdb_id",        d.imdb_id);
        applyStr(ov, "tmdb_id",        d.tmdb_id);
        applyStr(ov, "thumb",          d.thumb);
        applyStr(ov, "art",            d.art);
        applyStr(ov, "genres",         d.genres);
        applyStr(ov, "labels",         d.labels);
        applyStr(ov, "actors",         d.actors);
        applyStr(ov, "countries",      d.countries);
        applyStr(ov, "collections",    d.collections);
        applyInt(ov, "year",           d.year);
    }

    return d;
}

void ContentRepository::updateShow(const std::string& show_id,
                                    const std::vector<StrField>& str_fields,
                                    const std::vector<IntField>& int_fields) {
    if (str_fields.empty() && int_fields.empty()) return;
    std::string sql = "UPDATE show SET locked = 1";
    for (const auto& f : str_fields) sql += ", " + f.col + " = ?";
    for (const auto& f : int_fields) sql += ", " + f.col + " = ?";
    sql += " WHERE show_id = ?";
    SQLite::Statement s(db_.get(), sql);
    int idx = 1;
    for (const auto& f : str_fields) s.bind(idx++, f.val);
    for (const auto& f : int_fields) s.bind(idx++, f.val);
    s.bind(idx, show_id);
    s.exec();
}

void ContentRepository::updateMovie(const std::string& movie_id,
                                     const std::vector<StrField>& str_fields,
                                     const std::vector<IntField>& int_fields) {
    if (str_fields.empty() && int_fields.empty()) return;
    std::string sql = "UPDATE movie SET locked = 1";
    for (const auto& f : str_fields) sql += ", " + f.col + " = ?";
    for (const auto& f : int_fields) sql += ", " + f.col + " = ?";
    sql += " WHERE movie_id = ?";
    SQLite::Statement s(db_.get(), sql);
    int idx = 1;
    for (const auto& f : str_fields) s.bind(idx++, f.val);
    for (const auto& f : int_fields) s.bind(idx++, f.val);
    s.bind(idx, movie_id);
    s.exec();
}

void ContentRepository::setShowSkipScraping(const std::string& show_id, bool skip) {
    { SQLite::Statement s(db_.get(), "UPDATE show SET skip_scraping = ? WHERE show_id = ?");
      s.bind(1, skip ? 1 : 0); s.bind(2, show_id); s.exec(); }
    if (!skip) return;

    SQLite::Statement q(db_.get(), "SELECT match_status FROM show WHERE show_id = ?");
    q.bind(1, show_id);
    if (!q.executeStep()) return;
    const std::string status = q.getColumn(0).getString();
    if (status != "uncertain" && status != "unmatched") return;

    { SQLite::Statement d(db_.get(), "DELETE FROM item_match_candidate WHERE item_type='show' AND kairos_id = ?");
      d.bind(1, show_id); d.exec(); }
    { SQLite::Statement u(db_.get(), "UPDATE show SET match_status='unscraped', match_score=NULL WHERE show_id = ?");
      u.bind(1, show_id); u.exec(); }
}

void ContentRepository::setMovieSkipScraping(const std::string& movie_id, bool skip) {
    { SQLite::Statement s(db_.get(), "UPDATE movie SET skip_scraping = ? WHERE movie_id = ?");
      s.bind(1, skip ? 1 : 0); s.bind(2, movie_id); s.exec(); }
    if (!skip) return;

    SQLite::Statement q(db_.get(), "SELECT match_status FROM movie WHERE movie_id = ?");
    q.bind(1, movie_id);
    if (!q.executeStep()) return;
    const std::string status = q.getColumn(0).getString();
    if (status != "uncertain" && status != "unmatched") return;

    { SQLite::Statement d(db_.get(), "DELETE FROM item_match_candidate WHERE item_type='movie' AND kairos_id = ?");
      d.bind(1, movie_id); d.exec(); }
    { SQLite::Statement u(db_.get(), "UPDATE movie SET match_status='unscraped', match_score=NULL WHERE movie_id = ?");
      u.bind(1, movie_id); u.exec(); }
}

void ContentRepository::clearPendingMatchStateForLibrary(const std::string& library_id) {
    // Delete stale candidates before flipping match_status — both queries key
    // off the same "currently uncertain/unmatched" condition, so the DELETE
    // has to run while that's still true.
    { SQLite::Statement d(db_.get(), R"(
        DELETE FROM item_match_candidate
        WHERE item_type='show' AND kairos_id IN (
            SELECT s.show_id FROM show s
            JOIN source_mapping sm ON sm.kairos_id = s.show_id AND sm.item_type='show'
            WHERE sm.library_id = ? AND s.match_status IN ('uncertain','unmatched')
        )
      )");
      d.bind(1, library_id); d.exec(); }
    { SQLite::Statement u(db_.get(), R"(
        UPDATE show SET match_status='unscraped', match_score=NULL
        WHERE match_status IN ('uncertain','unmatched')
          AND show_id IN (SELECT kairos_id FROM source_mapping WHERE item_type='show' AND library_id=?)
      )");
      u.bind(1, library_id); u.exec(); }

    { SQLite::Statement d(db_.get(), R"(
        DELETE FROM item_match_candidate
        WHERE item_type='movie' AND kairos_id IN (
            SELECT m.movie_id FROM movie m
            JOIN source_mapping sm ON sm.kairos_id = m.movie_id AND sm.item_type='movie'
            WHERE sm.library_id = ? AND m.match_status IN ('uncertain','unmatched')
        )
      )");
      d.bind(1, library_id); d.exec(); }
    { SQLite::Statement u(db_.get(), R"(
        UPDATE movie SET match_status='unscraped', match_score=NULL
        WHERE match_status IN ('uncertain','unmatched')
          AND movie_id IN (SELECT kairos_id FROM source_mapping WHERE item_type='movie' AND library_id=?)
      )");
      u.bind(1, library_id); u.exec(); }
}

void ContentRepository::setShowFindSpecials(const std::string& show_id, bool find_specials) {
    SQLite::Statement s(db_.get(), "UPDATE show SET find_specials = ? WHERE show_id = ?");
    s.bind(1, find_specials ? 1 : 0);
    s.bind(2, show_id);
    s.exec();
}

void ContentRepository::setShowEpisodeDisplayOrder(const std::string& show_id, const std::string& order) {
    SQLite::Statement s(db_.get(), "UPDATE show SET episode_display_order = ? WHERE show_id = ?");
    s.bind(1, order);
    s.bind(2, show_id);
    s.exec();
}

void ContentRepository::mergeMovieInto(const std::string& target_id, const std::string& dup_id) {
    if (target_id == dup_id) throw std::runtime_error("cannot merge an item into itself");
    SQLite::Transaction txn(db_.get());

    // Move the dup's mappings onto the target, skipping any source the target already has.
    SQLite::Statement move(db_.get(), R"(
        UPDATE source_mapping SET kairos_id = ?
        WHERE item_type = 'movie' AND kairos_id = ?
          AND source_id NOT IN (
              SELECT source_id FROM source_mapping WHERE item_type = 'movie' AND kairos_id = ?
          )
    )");
    move.bind(1, target_id); move.bind(2, dup_id); move.bind(3, target_id);
    move.exec();

    SQLite::Statement dropLeftover(db_.get(),
        "DELETE FROM source_mapping WHERE item_type='movie' AND kairos_id=?");
    dropLeftover.bind(1, dup_id);
    dropLeftover.exec();

    // dup_id has zero mappings now — orphaned, same as SyncManager::runOrphanCleanup leaves a stale item.
    SQLite::Statement nullCursor(db_.get(), "UPDATE media_cursor SET movie_id = NULL WHERE movie_id = ?");
    nullCursor.bind(1, dup_id);
    nullCursor.exec();

    SQLite::Statement del(db_.get(), "DELETE FROM movie WHERE movie_id = ?");
    del.bind(1, dup_id);
    del.exec();

    txn.commit();
}

void ContentRepository::mergeShowInto(const std::string& target_id, const std::string& dup_id) {
    if (target_id == dup_id) throw std::runtime_error("cannot merge an item into itself");
    SQLite::Transaction txn(db_.get());

    // (season << 32 | episode) -> episode_id, so a matching dup episode collapses onto it.
    std::unordered_map<int64_t, std::string> target_ep_by_se;
    {
        SQLite::Statement q(db_.get(), "SELECT season, episode, episode_id FROM episode WHERE show_id = ?");
        q.bind(1, target_id);
        while (q.executeStep()) {
            const int64_t key = (static_cast<int64_t>(q.getColumn(0).getInt()) << 32)
                               | static_cast<uint32_t>(q.getColumn(1).getInt());
            target_ep_by_se[key] = q.getColumn(2).getString();
        }
    }

    struct DupEp { std::string episode_id; int season = 0, episode = 0; };
    std::vector<DupEp> dup_episodes;
    {
        SQLite::Statement q(db_.get(), "SELECT episode_id, season, episode FROM episode WHERE show_id = ?");
        q.bind(1, dup_id);
        while (q.executeStep())
            dup_episodes.push_back({q.getColumn(0).getString(), q.getColumn(1).getInt(), q.getColumn(2).getInt()});
    }

    SQLite::Statement moveEpMapping(db_.get(), R"(
        UPDATE source_mapping SET kairos_id = ?
        WHERE item_type = 'episode' AND kairos_id = ?
          AND source_id NOT IN (
              SELECT source_id FROM source_mapping WHERE item_type = 'episode' AND kairos_id = ?
          )
    )");
    SQLite::Statement dropEpMappingLeftover(db_.get(),
        "DELETE FROM source_mapping WHERE item_type='episode' AND kairos_id=?");
    SQLite::Statement nullCursorEp(db_.get(), "UPDATE media_cursor SET episode_id = NULL WHERE episode_id = ?");
    SQLite::Statement deleteEp(db_.get(), "DELETE FROM episode WHERE episode_id = ?");
    SQLite::Statement reparentEp(db_.get(), "UPDATE episode SET show_id = ? WHERE episode_id = ?");

    for (const auto& ep : dup_episodes) {
        const int64_t key = (static_cast<int64_t>(ep.season) << 32) | static_cast<uint32_t>(ep.episode);
        auto it = target_ep_by_se.find(key);
        if (it != target_ep_by_se.end()) {
            // Collision — fold the dup's sources onto the existing episode, drop the dup's row.
            const std::string& target_ep_id = it->second;
            moveEpMapping.bind(1, target_ep_id); moveEpMapping.bind(2, ep.episode_id); moveEpMapping.bind(3, target_ep_id);
            moveEpMapping.exec(); moveEpMapping.reset();
            dropEpMappingLeftover.bind(1, ep.episode_id); dropEpMappingLeftover.exec(); dropEpMappingLeftover.reset();
            nullCursorEp.bind(1, ep.episode_id); nullCursorEp.exec(); nullCursorEp.reset();
            deleteEp.bind(1, ep.episode_id); deleteEp.exec(); deleteEp.reset();
        } else {
            reparentEp.bind(1, target_id); reparentEp.bind(2, ep.episode_id);
            reparentEp.exec(); reparentEp.reset();
        }
    }

    SQLite::Statement moveShowMapping(db_.get(), R"(
        UPDATE source_mapping SET kairos_id = ?
        WHERE item_type = 'show' AND kairos_id = ?
          AND source_id NOT IN (
              SELECT source_id FROM source_mapping WHERE item_type = 'show' AND kairos_id = ?
          )
    )");
    moveShowMapping.bind(1, target_id); moveShowMapping.bind(2, dup_id); moveShowMapping.bind(3, target_id);
    moveShowMapping.exec();

    SQLite::Statement dropShowMappingLeftover(db_.get(),
        "DELETE FROM source_mapping WHERE item_type='show' AND kairos_id=?");
    dropShowMappingLeftover.bind(1, dup_id);
    dropShowMappingLeftover.exec();

    SQLite::Statement delSeason(db_.get(), "DELETE FROM show_season WHERE show_id = ?");
    delSeason.bind(1, dup_id);
    delSeason.exec();

    SQLite::Statement delShow(db_.get(), "DELETE FROM show WHERE show_id = ?");
    delShow.bind(1, dup_id);
    delShow.exec();

    txn.commit();
}

std::vector<EpisodeRow> ContentRepository::listEpisodesForShow(const std::string& show_id,
                                                                 const std::string& season_filter) {
    bool has_season = !season_filter.empty();
    SQLite::Statement q(db_.get(),
        std::string("SELECT episode_id, season, episode, title, duration_ms, overview, air_date, thumb, file_path "
                    "FROM episode WHERE show_id = ?") +
        (has_season ? " AND season = ?" : "") + " ORDER BY season, episode");
    q.bind(1, show_id);
    if (has_season) q.bind(2, std::stoi(season_filter));
    std::vector<EpisodeRow> rows;
    while (q.executeStep()) {
        EpisodeRow r;
        r.episode_id  = q.getColumn(0).getString();
        r.season      = q.getColumn(1).getInt();
        r.episode     = q.getColumn(2).getInt();
        r.title       = q.getColumn(3).getString();
        r.duration_ms = q.getColumn(4).getInt64();
        r.overview    = q.getColumn(5).getString();
        r.air_date    = q.getColumn(6).getString();
        r.thumb       = q.getColumn(7).getString();
        r.file_path   = q.getColumn(8).getString();
        rows.push_back(std::move(r));
    }
    return rows;
}

// Small, reusable building block — the most recently aired (real-world,
// not Kairos-channel) episode of a show, excluding blanks (unaired/special
// episodes commonly have no air_date at all) and future-dated episodes.
// Used to enrich Home's "Recently Aired" shelf with a jump-straight-to-
// episode target; the plain recently_aired *sort* on searchShows() above
// doesn't need this, it only needs the MAX(air_date) for ordering.
std::optional<EpisodeRow> ContentRepository::getLatestAiredEpisode(const std::string& show_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT episode_id, season, episode, title, duration_ms, overview, air_date, thumb
        FROM episode
        WHERE show_id = ? AND air_date != '' AND air_date <= date('now')
        ORDER BY air_date DESC LIMIT 1
    )");
    q.bind(1, show_id);
    if (!q.executeStep()) return std::nullopt;

    EpisodeRow r;
    r.episode_id  = q.getColumn(0).getString();
    r.season      = q.getColumn(1).getInt();
    r.episode     = q.getColumn(2).getInt();
    r.title       = q.getColumn(3).getString();
    r.duration_ms = q.getColumn(4).getInt64();
    r.overview    = q.getColumn(5).getString();
    r.air_date    = q.getColumn(6).getString();
    r.thumb       = q.getColumn(7).getString();
    return r;
}

// The next playable episode after episode_id — see header comment for the
// season/aired ordering rules. Includes linked specials (playable via a live
// join through linked_movie_id, see PlaybackService) but skips episodes with
// no file of their own and no linked movie.
std::optional<EpisodeRow> ContentRepository::getNextEpisode(const std::string& episode_id) {
    SQLite::Statement cur(db_.get(), R"(
        SELECT e.show_id, e.season, e.episode, e.air_date, s.episode_display_order
        FROM episode e JOIN show s ON s.show_id = e.show_id
        WHERE e.episode_id = ?
    )");
    cur.bind(1, episode_id);
    if (!cur.executeStep()) return std::nullopt;

    auto show_id  = cur.getColumn(0).getString();
    int  season   = cur.getColumn(1).getInt();
    int  episode  = cur.getColumn(2).getInt();
    auto air_date = cur.getColumn(3).getString();
    bool aired_order = cur.getColumn(4).getString() == "aired" && !air_date.empty();

    const std::string cols =
        "SELECT episode_id, season, episode, title, duration_ms, overview, air_date, thumb, file_path "
        "FROM episode WHERE show_id = ? AND (file_path != '' OR linked_movie_id IS NOT NULL) ";

    std::unique_ptr<SQLite::Statement> q;
    if (aired_order) {
        q = std::make_unique<SQLite::Statement>(db_.get(), cols +
            "AND air_date != '' AND air_date > ? ORDER BY air_date, season, episode LIMIT 1");
        q->bind(1, show_id);
        q->bind(2, air_date);
    } else {
        q = std::make_unique<SQLite::Statement>(db_.get(), cols +
            "AND (season > ? OR (season = ? AND episode > ?)) ORDER BY season, episode LIMIT 1");
        q->bind(1, show_id);
        q->bind(2, season);
        q->bind(3, season);
        q->bind(4, episode);
    }
    if (!q->executeStep()) return std::nullopt;

    EpisodeRow r;
    r.episode_id  = q->getColumn(0).getString();
    r.season      = q->getColumn(1).getInt();
    r.episode     = q->getColumn(2).getInt();
    r.title       = q->getColumn(3).getString();
    r.duration_ms = q->getColumn(4).getInt64();
    r.overview    = q->getColumn(5).getString();
    r.air_date    = q->getColumn(6).getString();
    r.thumb       = q->getColumn(7).getString();
    r.file_path   = q->getColumn(8).getString();
    return r;
}

std::vector<SeasonRow> ContentRepository::listSeasons(const std::string& show_id) {
    SQLite::Statement q(db_.get(),
        "SELECT DISTINCT e.season, COALESCE(ss.season_name, '') "
        "FROM episode e "
        "LEFT JOIN show_season ss ON ss.show_id = e.show_id AND ss.season = e.season "
        "WHERE e.show_id = ? ORDER BY e.season");
    q.bind(1, show_id);
    std::vector<SeasonRow> rows;
    while (q.executeStep())
        rows.push_back({q.getColumn(0).getInt(), q.getColumn(1).getString()});
    return rows;
}

std::vector<EpisodeSearchRow> ContentRepository::searchEpisodes(const std::string& show_id,
                                                                  const std::string& q_str,
                                                                  int season, int limit, int offset) {
    std::string where = " WHERE 1=1";
    if (!show_id.empty())  where += " AND e.show_id = ?";
    if (season >= 0)       where += " AND e.season = ?";
    if (!q_str.empty())    where += " AND (e.title LIKE '%' || ? || '%' OR s.title LIKE '%' || ? || '%')";

    SQLite::Statement q(db_.get(), R"(
        SELECT e.episode_id, e.season, e.episode, e.title, e.duration_ms,
               s.show_id, s.title AS show_title
        FROM episode e JOIN show s ON s.show_id = e.show_id
    )" + where + " ORDER BY s.title, e.season, e.episode LIMIT ? OFFSET ?");
    int idx = 1;
    if (!show_id.empty()) q.bind(idx++, show_id);
    if (season >= 0)      q.bind(idx++, season);
    if (!q_str.empty())   { q.bind(idx++, q_str); q.bind(idx++, q_str); }
    q.bind(idx++, limit); q.bind(idx++, offset);

    std::vector<EpisodeSearchRow> rows;
    while (q.executeStep()) {
        EpisodeSearchRow r;
        r.episode_id  = q.getColumn(0).getString();
        r.season      = q.getColumn(1).getInt();
        r.episode     = q.getColumn(2).getInt();
        r.title       = q.getColumn(3).getString();
        r.duration_ms = q.getColumn(4).getInt64();
        r.show_id     = q.getColumn(5).getString();
        r.show_title  = q.getColumn(6).getString();
        rows.push_back(std::move(r));
    }
    return rows;
}

static std::optional<ItemSource> getItemSource(Database& db, const std::string& sql,
                                                 const std::string& id) {
    SQLite::Statement q(db.get(), sql);
    q.bind(1, id);
    if (!q.executeStep()) return std::nullopt;
    std::string img = q.getColumn(0).getString();
    if (img.empty()) return std::nullopt;
    return ItemSource{img, q.getColumn(1).getString()};
}

std::optional<ItemSource> ContentRepository::getShowThumb(const std::string& show_id) {
    return getItemSource(db_, R"(
        SELECT s.thumb, COALESCE(sm.source_id, '') FROM show s
        LEFT JOIN source_mapping sm ON sm.item_type = 'show' AND sm.kairos_id = s.show_id
        WHERE s.show_id = ? LIMIT 1)", show_id);
}

std::optional<ItemSource> ContentRepository::getShowArt(const std::string& show_id) {
    return getItemSource(db_, R"(
        SELECT s.art, COALESCE(sm.source_id, '') FROM show s
        LEFT JOIN source_mapping sm ON sm.item_type = 'show' AND sm.kairos_id = s.show_id
        WHERE s.show_id = ? LIMIT 1)", show_id);
}

std::optional<ItemSource> ContentRepository::getEpisodeThumb(const std::string& episode_id) {
    return getItemSource(db_, R"(
        SELECT e.thumb, COALESCE(sm.source_id, '') FROM episode e
        LEFT JOIN source_mapping sm ON sm.item_type = 'episode' AND sm.kairos_id = e.episode_id
        WHERE e.episode_id = ? LIMIT 1)", episode_id);
}

std::optional<ItemSource> ContentRepository::getMovieThumb(const std::string& movie_id) {
    return getItemSource(db_, R"(
        SELECT m.thumb, COALESCE(sm.source_id, '') FROM movie m
        LEFT JOIN source_mapping sm ON sm.item_type = 'movie' AND sm.kairos_id = m.movie_id
        WHERE m.movie_id = ? LIMIT 1)", movie_id);
}

std::optional<ItemSource> ContentRepository::getMovieArt(const std::string& movie_id) {
    return getItemSource(db_, R"(
        SELECT m.art, COALESCE(sm.source_id, '') FROM movie m
        LEFT JOIN source_mapping sm ON sm.item_type = 'movie' AND sm.kairos_id = m.movie_id
        WHERE m.movie_id = ? LIMIT 1)", movie_id);
}

std::string ContentRepository::getSourceBaseUrl(const std::string& source_id) {
    SQLite::Statement q(db_.get(), "SELECT base_url FROM media_source WHERE source_id = ?");
    q.bind(1, source_id);
    if (q.executeStep()) return q.getColumn(0).getString();
    return {};
}
