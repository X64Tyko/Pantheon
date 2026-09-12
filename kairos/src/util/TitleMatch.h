#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Title normalization/similarity shared by ScraperManager (TMDB/TVDB
// candidate scoring) and SyncManager (cross-source sync-time dedup).
namespace titlematch
{
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

	// Detects a multi-part-movie file marker — "CD1"/"CD2", "Part 1"/"Part.2",
	// "pt.1", "Disc 1"/"Disc.2" (numeric only, by design, to keep false
	// positives rare) — in a raw on-disk filename. Returns the filename with
	// the marker removed (still raw — run it through parseReleaseTitle to get a
	// clean comparable base title) plus the 1-based part number, or nullopt
	// when no marker is present.
	//
	// Used by each source's fetchMovies() to group sibling files of the same
	// movie (e.g. "Movie.Title.CD1.mkv"/"Movie.Title.CD2.mkv") under one Movie
	// with populated `parts` instead of one Movie row per file — see GitHub #3.
	std::optional<std::pair<std::string, int>> detectFilePart(const std::string& raw);

	// Groups raw filenames/stems that appear to be sibling parts of one
	// multi-part movie: every stem must carry a detectFilePart() marker, all
	// markers must reduce to the same normalized base title, and there must be
	// at least 2 distinct part numbers. Returns the part number for each input
	// index (parallel to `stems`), or an empty vector when the input isn't a
	// consistent group — callers should then fall back to treating each stem
	// as its own separate item rather than risk grouping unrelated files (e.g.
	// a trailer alongside the main feature) into one bogus movie.
	//
	// Shared by LocalSource and JellyfinBaseSource (Plex exposes multi-part
	// files natively via Media.Part[] and doesn't need this) — see GitHub #3.
	std::vector<int> groupFileParts(const std::vector<std::string>& stems);
} // namespace titlematch