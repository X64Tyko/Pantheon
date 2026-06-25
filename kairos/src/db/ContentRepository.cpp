#include "ContentRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>

ContentRepository::ContentRepository(Database& db) : db_(db) {}

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

// ---------------------------------------------------------------------------
// Episodes
// ---------------------------------------------------------------------------

std::vector<Episode> ContentRepository::getEpisodes(const std::string& show_id,
                                                      std::optional<int> season,
                                                      bool include_specials,
                                                      const std::string& episode_order) {
    std::string sql =
        "SELECT episode_id, show_id, season, episode, title, file_path, duration_ms,"
        " overview, air_date, thumb"
        " FROM episode WHERE show_id = ?";
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
    while (q.executeStep()) eps.push_back(rowToEpisode(q));
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
        " e.duration_ms, e.overview, e.air_date, e.thumb"
        " FROM episode e WHERE e.show_id = ?";
    if (season)                       sql += " AND e.season = ?";
    if (!season && !include_specials) sql += " AND e.season != 0";
    sql += " AND EXISTS (SELECT 1 FROM play_history ph"
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
    while (q.executeStep()) eps.push_back(rowToEpisode(q));
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
        JOIN play_history ph ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.channel_id=? AND ph.aired_at < ?
        WHERE e.show_id=? AND e.season=?
        GROUP BY e.episode_id ORDER BY last_aired ASC
    )";
    const char* sql_ch_all = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.channel_id=? AND ph.aired_at < ?
        WHERE e.show_id=?
        GROUP BY e.episode_id ORDER BY last_aired ASC
    )";
    const char* sql_gl_season = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.aired_at < ?
        WHERE e.show_id=? AND e.season=?
        GROUP BY e.episode_id ORDER BY last_aired ASC
    )";
    const char* sql_gl_all = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph ON ph.item_type='episode' AND ph.item_id=e.episode_id
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
    const char* sql = (before_time > 0)
        ? "SELECT item_id, MAX(aired_at) FROM play_history WHERE channel_id=? AND aired_at<=? GROUP BY item_id"
        : "SELECT item_id, MAX(aired_at) FROM play_history WHERE channel_id=? GROUP BY item_id";
    SQLite::Statement q(db_.get(), sql);
    q.bind(1, channel_id);
    if (before_time > 0) q.bind(2, static_cast<int64_t>(before_time));
    std::unordered_map<std::string, int64_t> result;
    while (q.executeStep())
        result[q.getColumn(0).getString()] = q.getColumn(1).getInt64();
    return result;
}
