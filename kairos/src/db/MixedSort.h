#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace SQLite
{
	class Database;
}

// One show or movie's sort-relevant fields, for merging the two entities
// into a single interleaved order without a cross-entity SQL ORDER BY
// (which needs COALESCE/sentinel tricks to get NULL handling and direction
// right on two differently-shaped tables). Every dimension is fetched once
// regardless of which is active, so adding a new sort mode is a field here
// + a case in mixedRowLess, not a third place tracking which mode needs
// which column. Dates are YYYYMMDD ints (0 = unknown, sorts last), not
// "YYYY-MM-DD" strings, so every field is a plain number to compare.
struct MixedRow
{
	std::string entity_type; // "show" | "movie" | "episode"
	std::string id, title;
	int64_t year           = 0;
	double audience_rating = 0;
	int64_t added_at       = 0;
	double duration_ms     = 0;
	int64_t recent_key     = 0; // latest-aired-episode date (show) / release_date (movie) / air_date (episode)
	int64_t premiere_key   = 0; // originally_available_at (show) / release_date (movie) / air_date (episode)
};

// True if `a` sorts before `b` under this mode+direction. 0 (unknown)
// always sorts last on the date-keyed modes, regardless of direction.
bool mixedRowLess(const MixedRow& a, const MixedRow& b, const std::string& sort, bool desc);

// Natural (no explicit override) direction for a sort mode — callers with
// no direction toggle of their own (smart playlists) just pass this straight
// through; callers with one (Library page) override it from user input.
bool mixedRowNaturalDesc(const std::string& sort);

// Sorts (or shuffles, for "random", ignoring `desc`) in place.
void sortMixedRows(std::vector<MixedRow>& rows, const std::string& sort, bool desc);

// "YYYY-MM-DD" (or a subquery producing one) -> YYYYMMDD int, 0 if empty/NULL.
std::string mixedDateToInt(const std::string& col);

// Fetches every row from `table` (aliased `alias`) matching `where_sql`,
// with the entity-specific duration/recent/premiere-date expressions
// already computed (see e.g. dateToInt-wrapped subqueries for shows).
std::vector<MixedRow> fetchMixedRows(SQLite::Database& db, const std::string& entity_type, const std::string& id_col,
									 const std::string& table, const std::string& alias,
									 const std::string& where_sql, const std::vector<std::string>& binds,
									 const std::string& duration_expr, const std::string& recent_expr,
									 const std::string& premiere_expr);

// Episodes of shows matching `filter` (compiled against FilterEntity::Show —
// same canon grammar a smart playlist's filter_expr uses), as MixedRow
// entries (entity_type="episode", id=episode_id, title=the episode's own
// title) so they can merge directly into the same fetchMixedRows-produced
// list and sort via the exact same mixedRowLess/sortMixedRows machinery —
// no separate episode comparator needed. year/audience_rating/added_at are
// inherited from the parent show (an episode has no field of its own for
// any of them); recent_key and premiere_key are both the episode's own
// air_date (there's no "latest episode" concept for a single episode, so
// unlike a show's two distinct dates these collapse to one). A
// `watch_state:X` clause in `filter`, if present, applies per-episode
// (FilterExpr.h's episodeWatchCondition), not the whole-show interpretation
// compileFilterExpr gives Show rows elsewhere. Not sorted or limited — call
// sortMixedRows (and slice) yourself; kept separate so a caller merging this
// with another source (mixedEntityOrder's movies) can sort the combined set
// once. extra_where/extra_binds let a caller (Home-shelf/restriction
// scoping) layer on its own conditions without this module needing to know
// about RestrictionContext.
std::vector<MixedRow> fetchMixedEpisodeRows(SQLite::Database& db, const std::string& filter,
											const std::string& user_id,
											const std::string& extra_where              = "",
											const std::vector<std::string>& extra_binds = {});

// (entity_type, id) pairs for a "mixed" (show+movie) smart playlist filter,
// sorted and limited — used by both PlaylistRepository::refreshSmart and
// SyncManager::refreshSmartPlaylists (safe to share despite one running on
// a background thread: `db` is passed in per call, no shared connection or
// other mutable state lives in this module). Shows contribute individual
// episodes (via fetchMixedEpisodeRows), not the show itself — a "mixed"
// playlist's actual membership is always movie/episode (playlist_item has
// no "show" item type), so sorting shows by date and then appending each
// one's *entire* episode run at that position would bury this week's new
// episode under a show's whole back catalog, the same bug a show-typed date
// sort had before it got the equivalent fix (see PlaylistRepository's
// smart_expand_episodes). user_id scopes episode watch_state the same way
// PlaylistRepository::refreshSmart's does; empty (SyncManager's background
// pass, which has no specific viewer) makes a watch_state clause no-op.
// sort_dir: "" = the sort mode's own natural direction, "asc"/"desc"
// overrides it (ignored by "random").
std::vector<std::pair<std::string, std::string>> mixedEntityOrder(SQLite::Database& db, const std::string& filter_expr,
																  const std::string& sort, int limit,
																  const std::string& sort_dir = "",
																  const std::string& user_id  = "");