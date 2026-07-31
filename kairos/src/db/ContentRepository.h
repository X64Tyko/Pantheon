#pragma once
#include <cstdint>
#include <SQLiteCpp/SQLiteCpp.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "../model/Episode.h"
#include "../model/Movie.h"

class Database;

// Items loaded for filler/picker selection (type, id, duration).
struct FillerItem
{
	std::string item_type; // "episode" | "movie"
	std::string item_id;
	int64_t duration_ms = 0;
};

// ── API-layer result structs ──────────────────────────────────────────────────

struct LibraryRow
{
	std::string library_id, source_id, display_name, library_type;
	std::string source_name, source_type;
	bool show_on_home = true;
};

struct ShowRow
{
	std::string show_id, title, content_rating;
	int episode_count = 0;
	std::optional<int> year;
	std::string thumb, art, source_base_url, library_id;
	std::optional<double> audience_rating;
	std::string match_status;
	std::optional<double> match_score;
	// Distinct episodes of this show with a completed watch_progress entry
	// for the caller's user (0 if not logged in — see searchShows). Same
	// "count, not bool" shape as MovieRow::view_count, but there's no
	// per-show rewatch-count analogue the way a movie has one file to
	// rewatch — this is "how many episodes have you watched" instead.
	int watched_episode_count = 0;
};

struct ShowListResult
{
	std::vector<ShowRow> items;
	int total = 0;
};


struct SeasonRow
{
	int number = 0;
	std::string name;
};

struct ShowDetail
{
	std::string show_id, title, original_title, content_rating, overview, studio, status;
	std::string genres, tags, thumb, art, imdb_id, tvdb_id, tmdb_id;
	std::string originally_available_at;
	std::optional<int> year;
	std::optional<double> audience_rating;
	bool locked                       = false;
	bool skip_scraping                = false;
	bool find_specials                = false;
	std::string episode_display_order = "season"; // "season" | "aired"
	int episode_count                 = 0;
	// See ShowRow::watched_episode_count — same meaning, populated only when
	// getShowDetail() is called with a user_id.
	int watched_episode_count = 0;
	std::string labels, network, actors, countries, collections;
	std::string external_id, source_id, source_base_url;
	std::vector<SeasonRow> seasons;
	std::string match_status;
	std::optional<double> match_score;
	bool match_confirmed = false;
	// Common ancestor directory across all of this show's episode file_paths
	// (see ContentRepository::getShowFolderPath) — admin-facing "where does
	// this actually live on disk" display, the show-level analog of
	// MovieDetail::file_path. Empty if the show has no episodes with a
	// file_path yet.
	std::string folder_path;
};

struct EpisodeRow
{
	std::string episode_id;
	int season = 0, episode = 0;
	std::string title;
	int64_t duration_ms = 0;
	std::string overview, air_date, thumb;
	std::string file_path; // admin-facing "source file" display — not used for playback (see model/Episode.h for that)
	// Rewatch count for the caller's user (0 = never finished). watched is
	// just view_count > 0 — see ContentRepository::listEpisodesForShow.
	bool watched       = false;
	int64_t view_count = 0;
};

struct EpisodeSearchRow
{
	std::string episode_id;
	int season = 0, episode = 0;
	std::string title;
	int64_t duration_ms = 0;
	std::string show_id, show_title;
	std::string overview; // getEpisode() only — searchEpisodes() leaves this blank, no caller needs it there
};

// Library page's "Include Episodes" toggle (see LibraryStore.ts) — v1 scope
// deliberately does NOT extend the full canon rule-builder field set
// (genre/actor/studio/etc.) to episodes; see the FilterExpr.h comment for
// why. `q` is the existing fuzzy title/show-title search; `playlist_id`
// scopes to (and, with sort=="playlist_order", orders by) one playlist's
// episode members — the episode-side equivalent of the `playlist:<id>`
// FilterExpr field used for Show/Movie. No "recently_added" sort: episode
// has no added_at column (only show/movie do).
struct EpisodeSearchParams
{
	std::string show_id;
	std::string q;
	int season = -1; // -1 = unfiltered
	int limit  = 50, offset = 0;
	std::string sort;     // "title" (default, show/season/episode order) | "episode_number" (season/episode only, ignores show grouping) | "playlist_order"
	std::string sort_dir; // "" = natural direction for the sort mode; ignored by playlist_order (always position ASC)
	std::optional<std::string> playlist_id;
};

struct EpisodeListResult
{
	std::vector<EpisodeSearchRow> items;
	int total = 0;
};

struct MovieRow
{
	std::string movie_id, title, content_rating;
	int64_t duration_ms = 0;
	std::optional<int> year;
	std::string release_date; // "YYYY-MM-DD"; empty if the source never provided one
	std::string thumb, art, source_base_url, library_id;
	std::optional<double> audience_rating;
	std::string match_status;
	std::optional<double> match_score;
	// Rewatch count for the caller's user (0 = never finished). watched is
	// just view_count > 0 — see ContentRepository::searchMovies.
	bool watched       = false;
	int64_t view_count = 0;
};

struct MovieListResult
{
	std::vector<MovieRow> items;
	int total = 0;
};

struct MovieDetail
{
	std::string movie_id, title, content_rating;
	int64_t duration_ms = 0;
	std::optional<int> year;
	std::string release_date; // "YYYY-MM-DD"; empty if the source never provided one
	std::optional<double> audience_rating;
	bool locked        = false;
	bool skip_scraping = false;
	std::string overview, tagline, studio, director, writer, genres, tags, thumb, art, imdb_id, tmdb_id, original_title;
	std::string labels, actors, countries, collections;
	std::string external_id, source_id, source_base_url;
	std::string file_path; // admin-facing "source file" display
	std::string match_status;
	std::optional<double> match_score;
	bool match_confirmed = false;
	// Parent directory of file_path (see ContentRepository::parentDir) —
	// admin-facing "which folder is this in" display, alongside the raw file.
	std::string folder_path;
	// Rewatch count for the caller's user (0 = never finished). watched is
	// just view_count > 0 — see ContentRepository::getMovieDetail.
	bool watched       = false;
	int64_t view_count = 0;
};

struct ItemSource
{
	std::string image_path;
	std::string source_id;
};

// Parental-controls context for a search, decided by the service layer (which
// owns the "what does restricted mean" decision — see AuthContext/RatingSeverity)
// so the repository just applies a plain ceiling + override lookup.
struct RestrictionContext
{
	bool restricted    = false;
	int rating_ceiling = 0; // RatingSeverity::tvRatingSeverity/movieRatingSeverity(user's ceiling)
	std::string user_id;
};

// Sort/hydrate-by-id key — see searchMixedIndex/hydrateMixed.
struct MixedIndexEntry
{
	std::string content_type; // "show" | "movie" | "episode"
	std::string id, title;
};

// Render-ready tile — enough for a MediaCard, not the full ShowRow/MovieRow.
struct MixedTileRow
{
	std::string content_type; // "show" | "movie" | "episode"
	std::string id, title, thumb, art, library_id;
	std::optional<int> year;
	std::optional<double> audience_rating;
	// Movie: watched = view_count>0 (view_count is the rewatch count).
	// Show: watched = view_count>0 (view_count holds watched_episode_count
	// here; episode_count lets the UI show "3/12" for partial progress) —
	// same MediaCard convention as ShowRow/MovieRow, only populated when
	// hydrateMixed is given a user_id. Episode: watched = this one episode's
	// own completed state (not the parent show's aggregate).
	bool watched        = false;
	int64_t view_count  = 0;
	int episode_count   = 0;              // shows only, 0 for movies/episodes
	int64_t duration_ms = 0;              // episodes only
	int season          = 0, episode = 0; // episodes only
	std::string show_id, show_title;      // episodes only

	// Show tiles only, populated by callers that need "which episode to jump
	// to" (e.g. the Home "Recently Aired" shelf's play-latest-episode
	// itemAction) via getLatestAiredEpisode — mirrors the conditional N+1
	// ContentService.cpp's GET /api/shows already does for the same reason.
	// unset = not requested by the caller, not "this show has no episodes."
	struct LatestEpisodeRef
	{
		std::string episode_id, air_date;
		int season = 0, episode = 0;
	};

	std::optional<LatestEpisodeRef> latest_episode;
};

// No limit/offset — searchMixedIndex always returns every match; pagination
// is hydrating whatever slice of the sorted result a caller wants to render.
struct MixedSearchParams
{
	std::vector<std::string> library_ids;
	std::string filter;
	std::string sort;
	std::string sort_dir;
	bool home_only  = false;
	bool hide_empty = false;
	// false = movie-free (a smart_expand_episodes show-typed shelf's own
	// pure episode feed); true (default) = movies included, same as today.
	bool include_movies = true;
	// Shows contribute their individual episodes (via MixedSort.h's
	// fetchMixedEpisodeRows) instead of themselves — same "sort at the
	// actual displayed granularity, not shows-then-dump-the-whole-run"
	// reasoning as PlaylistRepository's smart_expand_episodes. Movies are
	// unaffected either way.
	bool expand_episodes = false;
	// rating_ceiling is on a different scale per entity_type (tvRatingSeverity
	// vs movieRatingSeverity), so this can't be a single shared context.
	RestrictionContext restriction_show, restriction_movie;
	std::string user_id;
};

struct ShowSearchParams
{
	int limit = 50, offset = 0;
	// Empty library_ids = unscoped/all libraries (fast path, no library JOIN).
	// A non-empty list scopes to exactly those libraries (multi-select
	// library switcher — see SourceSwitcher.tsx).
	std::vector<std::string> library_ids;
	// Canon filter syntax (see hades/src/components/media/filterSyntax.ts /
	// kairos/src/db/FilterExpr.h for the shared grammar) — replaces the old
	// flat per-field members (genre/year/content_rating/label/network/actor/
	// country/collection/studio) and the separate ad hoc `q` substring
	// search; free words in `filter` fold into fuzzy free-text matching.
	std::string filter;
	std::string sort; // "title" (default) | "recently_added" | "random" | "recently_aired" |
	// "year" | "audience_rating" | "duration" | "air_date" (chronological
	// premiere date) | "recently_released_or_aired" (alias of recently_aired
	// here — see searchMovies' identical alias for recently_released)
	// "" (default) = each sort mode's own natural direction (title: A-Z,
	// recently_added/recently_aired/recently_released: newest-first) —
	// "asc"/"desc" explicitly overrides it. Ignored by "random".
	std::string sort_dir;
	// Only used when sort == "random". Unset = plain ORDER BY RANDOM() (a
	// fresh shuffle every call, fine for e.g. a Home shelf). Set = a
	// deterministic pseudo-random order derived from (rowid, seed) — same
	// seed always produces the same order, so paginating (loadMore) through
	// a "Random" sorted browse doesn't reshuffle out from under the user;
	// the Library page's die/reroll button just picks a new seed.
	std::optional<int64_t> random_seed;
	// Only used when sort == "playlist_order" — which playlist's item
	// positions to sort by (see FilterExpr.cpp's separate `playlist:<id>`
	// filter field for *inclusion*; this is the orthogonal *ordering*
	// concern, since a compiled filter_expr string is opaque to the sort
	// builder). The Library page sends both together whenever a playlist
	// clause is active and Playlist Order is the selected sort.
	std::optional<std::string> playlist_id;
	RestrictionContext restriction;
	// Set only by the actual Home-page/Roku-home shelf loaders. Without it,
	// an unscoped (no library_id) search — the channel content picker,
	// filler/bumper pickers, Library page's "All Libraries" view — sees
	// every library including show_on_home=0 ones; home visibility is a
	// Home-shelf-only concept, not a general content filter.
	bool home_only = false;
	// watch_progress join key for ShowRow::watched_episode_count — see
	// MovieSearchParams::user_id's identical comment for why this isn't
	// restriction.user_id.
	std::string user_id;
	// Excludes shows with zero episodes — a matched-but-not-yet-synced (or
	// orphan-pruned-down-to-nothing) show that's really just a metadata
	// stub with no actual media. Always on for Home-shelf loaders (a shelf
	// tile with nothing playable behind it is just broken) and togglable on
	// the Library page (default on, flippable for admin/debugging visibility
	// into stub shows). Never set by the channel content picker or
	// filler/bumper pickers — a channel can still reference a still-empty
	// show so it automatically starts airing once episodes are synced in.
	bool hide_empty = false;
};

struct MovieSearchParams
{
	int limit = 50, offset = 0;
	std::vector<std::string> library_ids; // see ShowSearchParams::library_ids
	std::string filter;                   // see ShowSearchParams::filter
	std::string sort;                     // "title" (default) | "recently_added" | "random" | "recently_released" |
	// "year" | "audience_rating" | "duration" | "air_date" (chronological
	// release date) | "recently_released_or_aired" (alias of recently_released)
	// "" (default) = each sort mode's own natural direction (title: A-Z,
	// recently_added/recently_aired/recently_released: newest-first) —
	// "asc"/"desc" explicitly overrides it. Ignored by "random".
	std::string sort_dir;
	std::optional<int64_t> random_seed;     // see ShowSearchParams::random_seed
	std::optional<std::string> playlist_id; // see ShowSearchParams::playlist_id
	RestrictionContext restriction;
	bool home_only  = false; // see ShowSearchParams::home_only
	bool hide_empty = false; // see ShowSearchParams::hide_empty (movies: excludes a blank file_path)
	// watch_progress join key for MovieRow::watched/view_count — deliberately
	// NOT restriction.user_id, which ContentService::restrictionFor() only
	// populates for restricted users and leaves empty otherwise; reusing it
	// would silently break watched badges for every non-restricted account.
	std::string user_id;
};

struct StrField
{
	std::string col, val;
};

struct IntField
{
	std::string col;
	int val = 0;
};

class ContentRepository
{
public:
	explicit ContentRepository(Database& db);

	// Resolves item_type/item_id into the (entity_type, entity_id,
	// content_rating) triple RestrictionRepository::isAllowed actually
	// operates on. Episodes have no rating or override entity of their own —
	// they resolve to their parent show's, same as everywhere else in the
	// codebase treats episode rating (no per-episode content_rating field).
	struct RestrictionLookup
	{
		std::string entity_type, entity_id, content_rating;
	};

	RestrictionLookup resolveForRestriction(const std::string& item_type, const std::string& item_id);

	// ── Episodes ──────────────────────────────────────────────────────────────

	std::vector<Episode> getEpisodes(const std::string& show_id,
									 std::optional<int> season        = std::nullopt,
									 bool include_specials            = false,
									 const std::string& episode_order = "season");

	// ── Movies ────────────────────────────────────────────────────────────────

	std::optional<Movie> getMovie(const std::string& movie_id);

	// ── Lists (playlist / filler_list) ───────────────────────────────────────

	std::vector<std::pair<std::string, std::string>> loadListItems(const std::string& content_type, const std::string& content_id);

	// Filler items with duration, handling filler_list/playlist/show/movie content types.
	std::vector<FillerItem> loadFillerItems(const std::string& content_type,
											const std::string& content_id,
											std::optional<int> season_filter = std::nullopt);

	// ── Playlist show_collection helpers ─────────────────────────────────────

	std::string getPlaylistMode(const std::string& playlist_id);
	std::vector<std::string> getPlaylistShows(const std::string& playlist_id);
	std::vector<Episode> getPlaylistShowEpisodes(const std::string& playlist_id,
												 const std::string& show_id);

	// Number of items in a playlist (for cursor wrap-around).
	int getPlaylistItemCount(const std::string& playlist_id);

	// ── API-layer queries (used by ContentService) ────────────────────────────

	std::vector<LibraryRow> listLibraries();

	std::vector<std::string> getMetadataValues(const std::string& field,
											   const std::string& type,
											   const std::string& library_id);

	ShowListResult searchShows(const ShowSearchParams& p);
	MovieListResult searchMovies(const MovieSearchParams& p);

	// Truly interleaved show+movie result, in-memory sorted (see .cpp) —
	// always every match; page by hydrating a slice (hydrateMixed).
	std::vector<MixedIndexEntry> searchMixedIndex(const MixedSearchParams& p);

	// Tile data for exactly these (content_type, id) pairs, in that order.
	// user_id populates the watched fields when non-empty, same convention
	// as getShowDetail/getMovieDetail above.
	std::vector<MixedTileRow> hydrateMixed(const std::vector<std::pair<std::string, std::string>>& ids,
										   const RestrictionContext& restriction_show,
										   const RestrictionContext& restriction_movie,
										   const std::string& user_id = "");

	// Project a ShowRow/MovieRow down to the same render-ready tile shape
	// hydrateMixed() produces — lets a caller (see TvManifestService's
	// GET /api/tv/shelf-items) return one uniform tile shape regardless of
	// whether a shelf's content came from searchShows/searchMovies or the
	// mixed index/hydrate path, without duplicating field-by-field mapping
	// at each call site.
	static MixedTileRow toTile(const ShowRow& r);
	static MixedTileRow toTile(const MovieRow& r);

	// user_id populates ShowDetail::watched_episode_count when non-empty
	// (left at 0 otherwise) — same convention as getMovieDetail below.
	std::optional<ShowDetail> getShowDetail(const std::string& show_id, const std::string& user_id = "");
	// user_id populates MovieDetail::watched/view_count when non-empty (left
	// at their defaults otherwise) — see MovieSearchParams::user_id.
	std::optional<MovieDetail> getMovieDetail(const std::string& movie_id, const std::string& user_id = "");

	// match_confirmed show/movie ids eligible for "Writeback All" — never
	// includes an unconfirmed match, same gate as the single-item writeback
	// routes. library_id/source_id empty = unfiltered; non-empty scopes to
	// items with at least one source_mapping row in that library/source
	// (an item can be linked to more than one via cross-source merge, so
	// this is "belongs to," not "belongs only to").
	std::vector<std::string> getWritebackEligibleShowIds(const std::string& library_id,
														 const std::string& source_id);
	std::vector<std::string> getWritebackEligibleMovieIds(const std::string& library_id,
														  const std::string& source_id);

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

	// user_id populates EpisodeRow::watched/view_count when non-empty (left
	// at their defaults otherwise) — see MovieSearchParams::user_id.
	std::vector<EpisodeRow> listEpisodesForShow(const std::string& show_id,
												const std::string& season_filter = "",
												const std::string& user_id       = "");
	// The next playable episode after episode_id, honoring the show's
	// episode_display_order ('season': season/episode order; 'aired': air_date
	// order, falling back to season/episode if the current episode has no
	// air_date). Includes linked specials (playable via linked_movie_id) but
	// skips any episode with neither a file_path nor a linked_movie_id.
	// nullopt if episode_id doesn't exist or is the last playable episode.
	std::optional<EpisodeRow> getNextEpisode(const std::string& episode_id);
	// Most recently aired (real-world air_date, blanks/future excluded)
	// episode of a show — used by Home's Recently Aired shelf.
	std::optional<EpisodeRow> getLatestAiredEpisode(const std::string& show_id);
	std::vector<SeasonRow> listSeasons(const std::string& show_id);
	EpisodeListResult searchEpisodes(const EpisodeSearchParams& p);
	// Single episode by id, with its show's title and overview — nullopt if
	// it doesn't exist. EpisodeSearchRow (not EpisodeRow) specifically for
	// the show_id/show_title fields; used by admin-facing "what's this
	// device watching" views and Roku's hero panel, both of which only have
	// a bare episode_id to resolve into something displayable.
	std::optional<EpisodeSearchRow> getEpisode(const std::string& episode_id);

	std::optional<ItemSource> getShowThumb(const std::string& show_id);
	std::optional<ItemSource> getShowArt(const std::string& show_id);
	std::optional<ItemSource> getEpisodeThumb(const std::string& episode_id);
	std::optional<ItemSource> getMovieThumb(const std::string& movie_id);
	std::optional<ItemSource> getMovieArt(const std::string& movie_id);
	std::string getSourceBaseUrl(const std::string& source_id);

	// ── Metadata helpers ──────────────────────────────────────────────────────

	std::string showTitle(const std::string& show_id);

	// ── Episode group membership ──────────────────────────────────────────────

	// Returns ep_id → {group_id, part_num} for all episodes belonging to a group in show.
	std::unordered_map<std::string, std::pair<std::string, int>> getEpisodeGroupMap(const std::string& show_id);

	// If episode_id is a mid-group part (part_num > 1), returns the Part 1 episode_id.
	std::optional<std::string> findGroupPart1(const std::string& episode_id);

	// Content and filler SmartShuffle/LRU cooldown both moved to
	// CursorState::recentPlays — see RuleEngine::selectWeightedSmartCooldown/
	// smartShufflePool/pickFillerSim — so they're carried in the anchor instead
	// of re-derived from play_history/filler_play_history here.

private:
	Database& db_;

	static Episode rowToEpisode(SQLite::Statement& q);
	static Episode rowToEpisodeFull(SQLite::Statement& q);
};