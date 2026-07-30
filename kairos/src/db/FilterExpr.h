#pragma once
#include <optional>
#include <string>
#include <vector>

// Compiles the canon filter syntax (see hades/src/components/media/
// filterSyntax.ts for the shared grammar — this is a parallel C++
// implementation of the same language, not a shared codebase) into a SQL
// WHERE-clause boolean fragment, parameterized the same way the rest of
// ContentRepository does ('?' placeholders bound positionally against the
// returned `binds`, in order).
//
//   expr    := orExpr
//   orExpr  := andExpr ( ("OR"|"||") andExpr )*
//   andExpr := term ( ("AND"|"&&")? term )*      -- implicit AND between terms
//   term    := ("-"|"NOT") term | "(" expr ")" | clause | word
//   clause  := FIELD ":" (">=" | "<=" | ">" | "<")? VALUE
//   word    := bareword | "quoted phrase"         -- folds into fuzzy free text
//
// Malformed/partial input degrades gracefully (unknown "field:" prefixes
// fall back to a plain word, unbalanced parens are tolerated) since this is
// user-typed search-bar text, not a strict API contract.
struct FilterCompileResult
{
	std::string sql = "1=1";
	std::vector<std::string> binds;
};

enum class FilterEntity { Show, Movie };

// `alias` is the table alias already used by the calling query ("s" for
// show, "m" for movie, matching searchShows/searchMovies's existing SQL).
// `user_id` scopes the `watch_state` field (the only per-viewer field in the
// grammar) to one user's watch_progress rows. Left empty by callers that
// have no single viewer in context (smart-playlist refresh, which runs
// server-side for all users at once) — a `watch_state` clause degrades to a
// no-op there rather than failing, same as any other malformed input.
FilterCompileResult compileFilterExpr(const std::string& text, FilterEntity entity, const std::string& alias,
									  const std::string& user_id = "");

// Pulls just the `watch_state:X` value out of a filter string, if present
// (first match; ignores where in the expression it sits) — for callers that
// need to apply watch state at a different granularity than compileFilterExpr
// would (see PlaylistRepository::refreshSmart's show-typed episode expansion,
// which means "unwatched episodes of matching shows," not "shows nobody's
// started" — the whole-show interpretation the plain Show/Movie compile
// gives it).
std::optional<std::string> extractWatchState(const std::string& text);

// Episode-level "have I watched THIS episode" — the per-episode
// interpretation of a `watch_state:X` value (see extractWatchState above),
// as opposed to the whole-show interpretation compileFilterExpr gives Show
// rows. Empty user_id or no state = "1=1" (no-op), same convention as
// compileFilterExpr's own watch_state clause.
std::string episodeWatchCondition(const std::optional<std::string>& state, const std::string& user_id,
								  const std::string& episode_id_col, std::vector<std::string>& binds);