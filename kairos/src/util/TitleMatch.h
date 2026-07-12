#pragma once
#include <optional>
#include <string>
#include <utility>

// Title normalization/similarity shared by ScraperManager (TMDB/TVDB
// candidate scoring) and SyncManager (cross-source sync-time dedup).
namespace titlematch {

// Lowercases, strips a single leading "the "/"a "/"an ", then strips
// everything but alphanumerics and spaces.
std::string normalizeTitle(const std::string& s);

// Levenshtein similarity of the normalized titles, in [0,1].
double titleSimilarity(const std::string& a, const std::string& b);

// Parses a raw on-disk directory/file name into a clean search title +
// release year. Handles both tidy "Title (YYYY)" names and scene-release
// names like "The.Thing.1982.1080p.BluRay.x264-GROUP" or "Show Name
// Complete Season 1" (no year at all) — normalizes '.'/'_' separators,
// finds a release-year token, and cuts the title at whichever comes
// earliest: the year, a quality/source/codec/edition tag, or a
// collection/season descriptor ("Complete", "TV Series", "Seasons 1-3").
//
// Shared by LocalSource (parsing on-disk names into title+year at scan
// time) and ScraperManager (recovering a clean title from the on-disk
// folder name when a source-supplied title looks wrong — see
// matchShow/matchMovie's folder-mismatch check). Both need the exact same
// cleaning: if ScraperManager used a weaker strip, a messy real-world
// folder name would never look similar enough to the already-cleaned
// title, and the mismatch check would "recover" by searching with the
// raw junk-laden folder name instead.
std::pair<std::string, std::optional<int>> parseReleaseTitle(const std::string& raw);

} // namespace titlematch
