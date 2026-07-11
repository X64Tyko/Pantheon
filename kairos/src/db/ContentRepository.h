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
    bool show_on_home = true;
};

struct ShowRow {
    std::string show_id, title, content_rating;
    int episode_count = 0;
    std::optional<int>    year;
    std::string           thumb, art, source_base_url, library_id;
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
    bool skip_scraping = false;
    bool find_specials = false;
    std::string episode_display_order = "season"; // "season" | "aired"
    int  episode_count = 0;
    std::string labels, network, actors, countries, collections;
    std::string external_id, source_id, source_base_url;
    std::vector<SeasonRow> seasons;
    std::string           match_status;
    std::optional<double> match_score;
    bool                   match_confirmed = false;
    // Common ancestor directory across all of this show's episode file_paths
    // (see ContentRepository::getShowFolderPath) — admin-facing "where does
    // this actually live on disk" display, the show-level analog of
    // MovieDetail::file_path. Empty if the show has no episodes with a
    // file_path yet.
    std::string folder_path;
};

struct EpisodeRow {
    std::string episode_id;
    int season = 0, episode = 0;
    std::string title;
    int64_t duration_ms = 0;
    std::string overview, air_date, thumb;
    std::string file_path; // admin-facing "source file" display — not used for playback (see model/Episode.h for that)
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
    std::string           release_date; // "YYYY-MM-DD"; empty if the source never provided one
    std::string           thumb, art, source_base_url, library_id;
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
    std::string           release_date; // "YYYY-MM-DD"; empty if the source never provided one
    std::optional<double> audience_rating;
    bool locked = false;
    bool skip_scraping = false;
    std::string overview, tagline, studio, director, genres, thumb, art, imdb_id, tmdb_id;
    std::string labels, actors, countries, collections;
    std::string external_id, source_id, source_base_url;
    std::string           file_path; // admin-facing "source file" display
    std::string           match_status;
    std::optional<double> match_score;
    bool                   match_confirmed = false;
    // Parent directory of file_path (see ContentRepository::parentDir) —
    // admin-facing "which folder is this in" display, alongside the raw file.
    std::string folder_path;
};

struct ItemSource {
    std::string image_path;
    std::string source_id;
};

// Parental-controls context for a search, decided by the service layer (which
// owns the "what does restricted mean" decision — see AuthContext/RatingSeverity)
// so the repository just applies a plain ceiling + override lookup.
struct RestrictionContext {
    bool        restricted = false;
    int         rating_ceiling = 0; // RatingSeverity::tvRatingSeverity/movieRatingSeverity(user's ceiling)
    std::string user_id;
};

struct ShowSearchParams {
    int limit = 50, offset = 0;
    std::string library_id, q, genre, year, content_rating;
    std::string label, network, actor, country, collection, studio;
    std::string sort;   // "title" (default) | "recently_added" | "random" | "recently_aired"
    RestrictionContext restriction;
};

struct MovieSearchParams {
    int limit = 50, offset = 0;
    std::string library_id, q, genre, year, content_rating;
    std::string label, actor, country, collection, studio;
    std::string sort;   // "title" (default) | "recently_added" | "random" | "recently_released"
    RestrictionContext restriction;
};

struct StrField { std::string col, val; };
struct IntField { std::string col; int val = 0; };

class ContentRepository {
public:
    explicit ContentRepository(Database& db);

    // Resolves item_type/item_id into the (entity_type, entity_id,
    // content_rating) triple RestrictionRepository::isAllowed actually
    // operates on. Episodes have no rating or override entity of their own —
    // they resolve to their parent show's, same as everywhere else in the
    // codebase treats episode rating (no per-episode content_rating field).
    struct RestrictionLookup { std::string entity_type, entity_id, content_rating; };
    RestrictionLookup resolveForRestriction(const std::string& item_type, const std::string& item_id);

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

    // Strips the last path segment (filename) off a file path, e.g.
    // "/media/Movies/Foo (2020)/Foo.mkv" -> "/media/Movies/Foo (2020)".
    // Empty in, or no '/' found, -> empty out.
    static std::string parentDir(const std::string& file_path);

    // Show's root folder, derived from one episode's path (steps up past a
    // season subfolder if present). nullopt if no episode has a file_path.
    std::optional<std::string> getShowFolderPath(const std::string& show_id);

    void updateShow(const std::string& show_id,
                    const std::vector<StrField>& str_fields,
                    const std::vector<IntField>& int_fields);
    void updateMovie(const std::string& movie_id,
                     const std::vector<StrField>& str_fields,
                     const std::vector<IntField>& int_fields);

    // Toggles per-item scrape exemption. Deliberately separate from
    // updateShow/updateMovie above, which always set `locked = 1` as a side
    // effect (correct for metadata-field edits, wrong for this orthogonal
    // flag). Flipping to true also resets any pending 'uncertain'/'unmatched'
    // state back to 'unscraped' and clears stale candidates, so the item
    // disappears from the review queue immediately rather than waiting for
    // an unrelated match pass to notice.
    void setShowSkipScraping(const std::string& show_id, bool skip);
    void setMovieSkipScraping(const std::string& movie_id, bool skip);

    // Bulk version of the above cleanup, for when a whole library's
    // skip_scraping flag flips true — every mapped show/movie currently
    // 'uncertain'/'unmatched' resets to 'unscraped' with candidates cleared.
    void clearPendingMatchStateForLibrary(const std::string& library_id);

    // Same "separate from updateShow" reasoning as setShowSkipScraping above —
    // plain settings toggles, not metadata edits, so they shouldn't lock the record.
    void setShowFindSpecials(const std::string& show_id, bool find_specials);
    void setShowEpisodeDisplayOrder(const std::string& show_id, const std::string& order);

    // Manual link for cross-source duplicates auto-dedup missed. Moves dup_id's
    // source_mapping rows onto target_id, then deletes the orphaned dup_id.
    // Throws std::runtime_error if target_id == dup_id.
    void mergeMovieInto(const std::string& target_id, const std::string& dup_id);
    void mergeShowInto(const std::string& target_id, const std::string& dup_id);

    std::vector<EpisodeRow>       listEpisodesForShow(const std::string& show_id,
                                                       const std::string& season_filter = "");
    // The next playable episode after episode_id, honoring the show's
    // episode_display_order ('season': season/episode order; 'aired': air_date
    // order, falling back to season/episode if the current episode has no
    // air_date). Includes linked specials (playable via linked_movie_id) but
    // skips any episode with neither a file_path nor a linked_movie_id.
    // nullopt if episode_id doesn't exist or is the last playable episode.
    std::optional<EpisodeRow>     getNextEpisode(const std::string& episode_id);
    // Most recently aired (real-world air_date, blanks/future excluded)
    // episode of a show — used by Home's Recently Aired shelf.
    std::optional<EpisodeRow>     getLatestAiredEpisode(const std::string& show_id);
    std::vector<SeasonRow>        listSeasons(const std::string& show_id);
    std::vector<EpisodeSearchRow> searchEpisodes(const std::string& show_id,
                                                  const std::string& q,
                                                  int season, int limit, int offset);
    // Single episode by id, with its show's title — nullopt if it doesn't
    // exist. EpisodeSearchRow (not EpisodeRow) specifically for the
    // show_id/show_title fields; used by admin-facing "what's this device
    // watching" views (connected-devices list) that only have a bare
    // episode_id to resolve into something displayable.
    std::optional<EpisodeSearchRow> getEpisode(const std::string& episode_id);

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
