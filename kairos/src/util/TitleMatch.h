#pragma once
#include <string>

// Title normalization/similarity shared by ScraperManager (TMDB/TVDB
// candidate scoring) and SyncManager (cross-source sync-time dedup).
namespace titlematch {

// Lowercases, strips a single leading "the "/"a "/"an ", then strips
// everything but alphanumerics and spaces.
std::string normalizeTitle(const std::string& s);

// Levenshtein similarity of the normalized titles, in [0,1].
double titleSimilarity(const std::string& a, const std::string& b);

} // namespace titlematch
