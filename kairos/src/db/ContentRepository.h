#pragma once
#include <cstdint>
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../model/Episode.h"
#include "../model/Movie.h"

class Database;

// Items loaded for filler/picker selection (type, id, duration).
struct FillerItem {
    std::string item_type;  // "episode" | "movie"
    std::string item_id;
    int64_t     duration_ms = 0;
};

// ── API-layer result structs ──────────────────────────────────────────────────

struct LibraryRow {
    std::string library_id, source_id, display_name, library_type;
    std::string source_name, source_type;
};

struct ShowRow {
    std::string show_id, title, content_rating;
    int episode_count = 0;
    std::optional<int>    year;
    std::string           thumb, art, source_base_url;
    std::optional<double> audience_rating;
    std::string           match_status;
    std::optional<double> match_score;
};

struct ShowListResult {
    std::vector<ShowRow> items;
    int total = 0;
};

struct SeasonRow {
    int number = 0;
    std::string name;
};

struct ShowDetail {
    std::string show_id, title, content_rating, overview, studio, status;
    std::string genres, thumb, art, imdb_id, tvdb_id, tmdb_id;
    std::string originally_available_at;
    std::optional<int>    year;
    std::optional<double> audience_rating;
    bool locked = false;
    int  episode_count = 0;
    std::string labels, network, actors, countries, collections;
    std::string external_id, source_id, source_base_url;
    std::vector<SeasonRow> seasons;
};

struct EpisodeRow {
    std::string episode_id;
    int season = 0, episode = 0;
    std::string title;
    int64_t duration_ms = 0;
    std::string overview, air_date, thumb;
};

struct EpisodeSearchRow {
    std::string episode_id;
    int season = 0, episode = 0;
    std::string title;
    int64_t duration_ms = 0;
    std::string show_id, show_title;
};

struct MovieRow {
    std::string movie_id, title, content_rating;
    int64_t duration_ms = 0;
    std::optional<int>    year;
    std::string           thumb, art, source_base_url;
    std::optional<double> audience_rating;
    std::string           match_status;
    std::optional<double> match_score;
};

struct MovieListResult {
    std::vector<MovieRow> items;
    int total = 0;
};

struct MovieDetail {
    std::string movie_id, title, content_rating;
    int64_t duration_ms = 0;
    std::optional<int>    year;
    std::optional<double> audience_rating;
    bool locked = false;
    std::string overview, tagline, studio, director, genres, thumb, art, imdb_id, tmdb_id;
    std::string labels, actors, countries, collections;
    std::string external_id, source_id, source_base_url;
};

struct ItemSource {
    std::string image_path;
    std::string source_id;
};

struct ShowSearchParams {
    int limit = 50, offset = 0;
    std::string library_id, q, genre, year, content_rating;
    std::string label, network, actor, country, collection, studio;
    std::string sort;   // "title" (default) | "recently_added" | "random"
};

struct MovieSearchParams {
    int limit = 50, offset = 0;
    std::string library_id, q, genre, year, content_rating;
    std::string label, actor, country, collection, studio;
    std::string sort;   // "title" (default) | "recently_added" | "random"
};

struct StrField { std::string col, val; };
struct IntField { std::string col; int val = 0; };

class ContentRepository {
public:
    explicit ContentRepository(Database& db);

    // ── Episodes ──────────────────────────────────────────────────────────────

    std::vector<Episode> getEpisodes(const std::string& show_id,
                                     std::optional<int> season = std::nullopt,
                                     bool include_specials = false,
                                     const std::string& episode_order = "season");

    // Episodes with play history (before_time) in the given channel scope.
    std::vector<Episode> getPlayedEpisodes(const std::string& show_id,
                                           const std::string& channel_id,
                                           std::optional<int> season,
                                           std::time_t before_time,
                                           bool global_scope = false,
                                           bool include_specials = false,
                                           const std::string& episode_order = "season");

    // Like getPlayedEpisodes but trims the most-recently-played smart_pct% from the pool.
    std::vector<Episode> getPlayedEpisodesWithCooldown(const std::string& show_id,
                                                       const std::string& channel_id,
                                                       std::optional<int> season,
                                                       int smart_pct,
                                                       std::time_t before_time,
                                                       bool global_scope = false,
                                                       bool include_specials = false);

    // ── Movies ────────────────────────────────────────────────────────────────

    std::optional<Movie> getMovie(const std::string& movie_id);

    // ── Lists (playlist / filler_list) ───────────────────────────────────────

    std::vector<std::pair<std::string, std::string>>
        loadListItems(const std::string& content_type, const std::string& content_id);

    // Filler items with duration, handling filler_list/playlist/show/movie content types.
    std::vector<FillerItem> loadFillerItems(const std::string& content_type,
                                             const std::string& content_id,
                                             std::optional<int> season_filter = std::nullopt);

    // ── Playlist show_collection helpers ─────────────────────────────────────

    std::string              getPlaylistMode(const std::string& playlist_id);
    std::vector<std::string> getPlaylistShows(const std::string& playlist_id);
    std::vector<Episode>     getPlaylistShowEpisodes(const std::string& playlist_id,
                                                      const std::string& show_id);

    // Number of items in a playlist (for cursor wrap-around).
    int getPlaylistItemCount(const std::string& playlist_id);

    // ── API-layer queries (used by ContentService) ────────────────────────────

    std::vector<LibraryRow> listLibraries();

    std::vector<std::string> getMetadataValues(const std::string& field,
                                                const std::string& type,
                                                const std::string& library_id);

    ShowListResult  searchShows(const ShowSearchParams& p);
    MovieListResult searchMovies(const MovieSearchParams& p);

    std::optional<ShowDetail>  getShowDetail(const std::string& show_id);
    std::optional<MovieDetail> getMovieDetail(const std::string& movie_id);

    void updateShow(const std::string& show_id,
                    const std::vector<StrField>& str_fields,
                    const std::vector<IntField>& int_fields);
    void updateMovie(const std::string& movie_id,
                     const std::vector<StrField>& str_fields,
                     const std::vector<IntField>& int_fields);

    std::vector<EpisodeRow>       listEpisodesForShow(const std::string& show_id,
                                                       const std::string& season_filter = "");
    std::vector<SeasonRow>        listSeasons(const std::string& show_id);
    std::vector<EpisodeSearchRow> searchEpisodes(const std::string& show_id,
                                                  const std::string& q,
                                                  int season, int limit, int offset);

    std::optional<ItemSource> getShowThumb(const std::string& show_id);
    std::optional<ItemSource> getShowArt(const std::string& show_id);
    std::optional<ItemSource> getEpisodeThumb(const std::string& episode_id);
    std::optional<ItemSource> getMovieThumb(const std::string& movie_id);
    std::optional<ItemSource> getMovieArt(const std::string& movie_id);
    std::string               getSourceBaseUrl(const std::string& source_id);

    // ── Metadata helpers ──────────────────────────────────────────────────────

    std::string showTitle(const std::string& show_id);

    // ── Episode group membership ──────────────────────────────────────────────

    // Returns ep_id → {group_id, part_num} for all episodes belonging to a group in show.
    std::unordered_map<std::string, std::pair<std::string, int>>
        getEpisodeGroupMap(const std::string& show_id);

    // If episode_id is a mid-group part (part_num > 1), returns the Part 1 episode_id.
    std::optional<std::string> findGroupPart1(const std::string& episode_id);

    // ── Play-history hot-ID queries ───────────────────────────────────────────

    // Most recently played movie IDs (hot set for SmartShuffle cooldown).
    std::unordered_set<std::string> getHotMovieIds(const std::string& channel_id,
                                                    std::time_t before_time,
                                                    int limit);

    // Most recently played episode IDs for a given show (hot set for SmartShuffle).
    std::unordered_set<std::string> getHotEpisodeIds(const std::string& channel_id,
                                                      std::time_t before_time,
                                                      const std::string& show_id,
                                                      int limit);

    // Recency map: item_id → last aired_at (for "sized" filler selection).
    std::unordered_map<std::string, int64_t> getLastPlayedMap(const std::string& channel_id,
                                                               std::time_t before_time);

private:
    Database& db_;

    static Episode rowToEpisode(SQLite::Statement& q);
    static Episode rowToEpisodeFull(SQLite::Statement& q);
};
