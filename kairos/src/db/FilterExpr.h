#pragma once
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
struct FilterCompileResult {
    std::string sql = "1=1";
    std::vector<std::string> binds;
};

enum class FilterEntity { Show, Movie };

// `alias` is the table alias already used by the calling query ("s" for
// show, "m" for movie, matching searchShows/searchMovies's existing SQL).
FilterCompileResult compileFilterExpr(const std::string& text, FilterEntity entity, const std::string& alias);
