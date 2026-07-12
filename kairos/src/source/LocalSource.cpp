#include "LocalSource.h"
#include "conf/ConfStore.h"
#include "model/Episode.h"
#include "model/Movie.h"
#include "model/Show.h"
#include "util/TitleMatch.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

namespace {

const std::unordered_set<std::string> kVideoExts = {
    ".mkv", ".mp4", ".avi", ".m4v", ".mov", ".wmv",
    ".flv", ".ts", ".mpg", ".mpeg", ".m2ts", ".webm",
};

// S01E01, S1E1 — or 1x01, 1x1 (common alt notation)
const std::regex kEpisodeRe(
    R"(S(\d{1,2})E(\d{1,3})|(\d{1,2})[xX](\d{1,3}))",
    std::regex::icase
);

// Season directory name: "Season 1", "Season 01", "S01", "Series 2", etc.
const std::regex kSeasonDirRe(
    R"((?:Season|Series|S(?:eason)?)\s*0*(\d+))",
    std::regex::icase
);

bool isVideo(const fs::path& p) {
    if (!fs::is_regular_file(p)) return false;
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return kVideoExts.count(ext) > 0;
}

bool isHidden(const fs::path& p) {
    const std::string name = p.filename().string();
    return !name.empty() && name[0] == '.';
}

// Parse a directory or file stem into a clean search title + release year.
// Handles both tidy "Title (YYYY)" names and scene-release names like
// "The.Thing.1982.1080p.BluRay.x264-GROUP" or "The Toxic Avenger UNRATED
// BDRip x264-GROUP" (no year at all). Anything from the year token / first
// recognised quality-or-edition tag onward is dropped as not part of the title.
// Shared with ScraperManager's folder-mismatch recovery — see TitleMatch.h.
std::pair<std::string, std::optional<int>> parseTitle(const std::string& raw) {
    return titlematch::parseReleaseTitle(raw);
}

// Returns the season index from a directory name, or -1 if not a season dir.
int parseSeasonDir(const std::string& name) {
    std::smatch m;
    if (std::regex_search(name, m, kSeasonDirRe))
        return std::stoi(m[1].str());
    return -1;
}

struct EpisodeLoc { int season = 0; int episode = 0; std::string title; };

std::optional<EpisodeLoc> parseEpisodeFilename(const std::string& stem) {
    std::smatch m;
    if (!std::regex_search(stem, m, kEpisodeRe)) return std::nullopt;

    EpisodeLoc loc;
    if (m[1].matched) { loc.season = std::stoi(m[1].str()); loc.episode = std::stoi(m[2].str()); }
    else              { loc.season = std::stoi(m[3].str()); loc.episode = std::stoi(m[4].str()); }

    // Title: text after the match, stripping a leading " - " or "."
    std::string after = stem.substr(static_cast<size_t>(m.position()) + m.length());
    static const std::regex kSep(R"(^\s*[-–.]\s*)");
    after = std::regex_replace(after, kSep, "");
    while (!after.empty() && (after.back() == ' ' || after.back() == '-')) after.pop_back();
    loc.title = after;
    return loc;
}

// Non-recursive: collect video files directly inside dir, sorted by name.
std::vector<fs::path> videosIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (isVideo(e.path())) out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

// Whether `dir` structurally looks like a show folder: a lone season-named
// directory itself, a season subdirectory one level deeper, or a flat layout
// (no season wrapper) with multiple video files where at least one filename
// carries an episode number. Shared by guessLibraryType() (initial per-library
// suggestion) and fetchShows()/fetchMovies() (per-subdirectory routing inside
// an actual "mixed" library) so both agree on what counts as a show.
bool looksLikeShowDir(const fs::path& dir) {
    if (parseSeasonDir(dir.filename().string()) >= 0) return true;

    std::error_code ec;
    int video_count = 0;
    bool has_episode_numbered_video = false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (isHidden(e.path())) continue;
        if (e.is_directory()) {
            if (parseSeasonDir(e.path().filename().string()) >= 0) return true;
        } else if (isVideo(e.path())) {
            ++video_count;
            if (std::regex_search(e.path().stem().string(), kEpisodeRe))
                has_episode_numbered_video = true;
        }
    }
    return video_count >= 2 && has_episode_numbered_video;
}

// Whether `dir` structurally looks like a movie folder: has video file(s)
// directly inside, and doesn't already look like a show.
bool looksLikeMovieDir(const fs::path& dir) {
    return !looksLikeShowDir(dir) && !videosIn(dir).empty();
}

} // namespace

// ---------------------------------------------------------------------------

LocalSource::LocalSource(const std::string& source_id, const std::string& base_path, ConfStore& conf)
    : source_id_(source_id), base_path_(base_path), conf_(conf) {}

// ---------------------------------------------------------------------------
// Library discovery — each non-hidden immediate subdirectory of base_path_
// is offered as a separately-configurable library.  Type is auto-detected
// with a two-level heuristic:
//   • Any grandchild dir matches kSeasonDirRe  → "show"
//   • Any child dir has video files, or a video file sits at root level → "movie"
//   • Otherwise → "mixed"
// ---------------------------------------------------------------------------

// Tallies show/movie evidence across every child instead of returning on the
// first match — a single stray subdirectory that happens to look season-shaped
// (a boxset extra, an oddly-named folder, even a movie title starting with
// "S1...") no longer flips an entire library of otherwise-clean movie folders
// to "show". Both signals present → "mixed" (safe: fetchShows()/fetchMovies()
// route each subdirectory individually in that case, see below). Neither
// signal → "mixed" too, same safe fallback as the previous empty-library case.
static std::string guessLibraryType(const fs::path& dir) {
    std::error_code ec;
    bool hasShowSignal = false, hasMovieSignal = false;
    int scanned = 0;
    // Bounded so a library with thousands of entries — especially over
    // network/FUSE-backed storage (e.g. Unraid's shfs), where every
    // directory read costs real round-trip latency — can't turn a single UI
    // browse click into a multi-second-or-worse stall. A few hundred
    // children is already far more evidence than the single early-return
    // this tally replaced ever looked at, so the cap doesn't reintroduce
    // that bug. Stops even earlier once both signals are already confirmed,
    // since no further child can change a "mixed" verdict.
    constexpr int kMaxScan = 300;
    for (const auto& child : fs::directory_iterator(dir, ec)) {
        if (isHidden(child.path())) continue;
        if (child.is_directory()) {
            // looksLikeShowDir() is checked once and reused directly here
            // instead of via looksLikeMovieDir() (which would re-run it) —
            // avoids scanning the same subdirectory's contents twice.
            if (looksLikeShowDir(child.path())) hasShowSignal = true;
            else if (!videosIn(child.path()).empty()) hasMovieSignal = true;
        } else if (isVideo(child.path())) {
            hasMovieSignal = true;
        }
        if (hasShowSignal && hasMovieSignal) break;
        if (++scanned >= kMaxScan) break;
    }
    if (hasShowSignal && hasMovieSignal) return "mixed";
    if (hasShowSignal)  return "show";
    if (hasMovieSignal) return "movie";
    return "mixed";
}

std::vector<LibraryInfo> LocalSource::listAvailableLibraries() {
    std::error_code ec;
    const fs::path root(base_path_);
    if (!fs::is_directory(root, ec)) {
        std::cerr << "[local:" << source_id_ << "] not a directory: " << base_path_ << '\n';
        return {};
    }

    std::vector<LibraryInfo> result;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory() || isHidden(entry.path())) continue;
        LibraryInfo info;
        info.external_lib_id = entry.path().string();
        info.name            = entry.path().filename().string();
        info.type            = guessLibraryType(entry.path());
        result.push_back(std::move(info));
    }
    std::sort(result.begin(), result.end(), [](const LibraryInfo& a, const LibraryInfo& b) {
        return a.name < b.name;
    });

    // If there are no subdirectories, fall back to the root itself.
    if (result.empty()) {
        LibraryInfo info;
        info.external_lib_id = base_path_;
        info.name            = root.filename().string();
        if (info.name.empty()) info.name = base_path_;
        info.type            = "mixed";
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<LibraryInfo> LocalSource::listSubdirectories(const std::string& path) {
    std::error_code ec;
    const fs::path target(path.empty() ? base_path_ : path);
    const fs::path base(base_path_);

    // Reject any path that escapes base_path_.
    auto normTarget = fs::weakly_canonical(target, ec);
    auto normBase   = fs::weakly_canonical(base, ec);
    auto rel = normTarget.lexically_relative(normBase);
    if (rel.empty() || rel.native().rfind("..", 0) == 0) return {};

    if (!fs::is_directory(normTarget, ec)) return {};

    std::vector<LibraryInfo> result;
    for (const auto& entry : fs::directory_iterator(normTarget, ec)) {
        if (!entry.is_directory() || isHidden(entry.path())) continue;
        LibraryInfo info;
        info.external_lib_id = entry.path().string();
        info.name            = entry.path().filename().string();
        info.type            = guessLibraryType(entry.path());
        result.push_back(std::move(info));
    }
    std::sort(result.begin(), result.end(), [](const LibraryInfo& a, const LibraryInfo& b) {
        return a.name < b.name;
    });
    return result;
}

// ---------------------------------------------------------------------------
// Shows — each non-hidden top-level subdirectory is a show validte with looksLikeShowDir.
// ---------------------------------------------------------------------------

std::vector<Show> LocalSource::fetchShows(const std::string& external_lib_id) {
    std::error_code ec;
    if (!fs::is_directory(external_lib_id, ec)) return {};

    std::vector<Show> result;
    for (const auto& entry : fs::directory_iterator(external_lib_id, ec)) {
        if (!entry.is_directory() || isHidden(entry.path())) continue;
        if (!looksLikeShowDir(entry.path())) continue;
        auto [title, year] = parseTitle(entry.path().filename().string());
        Show show;
        show.show_id     = conf_.applyPathMap(entry.path().string()); // mapped path as external key
        show.title       = title;
        show.folder_path = entry.path().string(); // raw, unmapped — mapping happens at dedup-compare time
        show.genres      = "[]";
        show.labels      = "[]";
        show.actors      = "[]";
        show.countries   = "[]";
        show.collections = "[]";
        if (year) show.year = year;
        result.push_back(std::move(show));
    }
    std::sort(result.begin(), result.end(), [](const Show& a, const Show& b) {
        return a.title < b.title;
    });
    return result;
}

// ---------------------------------------------------------------------------
// Movies — subdirectory-per-movie or bare video files at root level.
// ---------------------------------------------------------------------------

std::vector<Movie> LocalSource::fetchMovies(const std::string& external_lib_id) {
    std::error_code ec;
    if (!fs::is_directory(external_lib_id, ec)) return {};

    std::vector<Movie> result;
    for (const auto& entry : fs::directory_iterator(external_lib_id, ec)) {
        const fs::path p = entry.path();
        if (isHidden(p)) continue;

        if (entry.is_directory()) {
            if (!looksLikeMovieDir(p)) continue;
            auto vfiles = videosIn(p);
            if (vfiles.empty()) continue;
            auto [title, year] = parseTitle(p.filename().string());
            Movie movie;
            movie.movie_id    = conf_.applyPathMap(p.string());
            movie.title       = title;
            movie.file_path   = conf_.applyPathMap(vfiles.front().string());
            movie.genres      = "[]";
            movie.labels      = "[]";
            movie.actors      = "[]";
            movie.countries   = "[]";
            movie.collections = "[]";
            if (year) movie.year = year;
            result.push_back(std::move(movie));
        } else if (isVideo(p)) {
            auto [title, year] = parseTitle(p.stem().string());
            Movie movie;
            movie.movie_id    = conf_.applyPathMap(p.string());
            movie.title       = title;
            movie.file_path   = conf_.applyPathMap(p.string());
            movie.genres      = "[]";
            movie.labels      = "[]";
            movie.actors      = "[]";
            movie.countries   = "[]";
            movie.collections = "[]";
            if (year) movie.year = year;
            result.push_back(std::move(movie));
        }
    }
    std::sort(result.begin(), result.end(), [](const Movie& a, const Movie& b) {
        return a.title < b.title;
    });
    return result;
}

// ---------------------------------------------------------------------------
// Episodes — thread-safe; only reads filesystem, no shared mutable state.
// external_show_id is the directory path returned by fetchShows().
// ---------------------------------------------------------------------------

std::vector<Episode> LocalSource::fetchEpisodes(const std::string& external_show_id) {
    std::error_code ec;
    if (!fs::is_directory(external_show_id, ec)) return {};

    const fs::path showDir(external_show_id);
    std::vector<Episode> result;

    // The caller (SyncManager) passes us the mapped show path if it was mapped,
    // but we should still handle files inside consistently.
    // However, external_show_id should already be mapped by SyncManager or LocalSource::fetchShows.

    // Collect season subdirectories (recognised by kSeasonDirRe).
    std::vector<fs::path> seasonDirs;
    for (const auto& e : fs::directory_iterator(showDir, ec)) {
        if (!e.is_directory() || isHidden(e.path())) continue;
        if (parseSeasonDir(e.path().filename().string()) >= 0)
            seasonDirs.push_back(e.path());
    }
    std::sort(seasonDirs.begin(), seasonDirs.end());

    auto addEpisode = [&](const fs::path& file, int season_hint) {
        auto loc = parseEpisodeFilename(file.stem().string());
        Episode ep;
        std::string mapped_file = conf_.applyPathMap(file.string());
        ep.episode_id = mapped_file;
        ep.show_id    = external_show_id;
        ep.file_path  = mapped_file;
        if (loc) {
            ep.season  = loc->season;
            ep.episode = loc->episode;
            ep.title   = loc->title;
        } else {
            ep.season  = (season_hint > 0) ? season_hint : 1;
            ep.episode = 0;
            ep.title   = file.stem().string();
        }
        result.push_back(std::move(ep));
    };

    if (!seasonDirs.empty()) {
        for (const auto& sdir : seasonDirs) {
            const int snum = parseSeasonDir(sdir.filename().string());
            for (const auto& f : videosIn(sdir))
                addEpisode(f, snum);
        }
    } else {
        // Flat layout: all video files sit directly in the show directory.
        for (const auto& f : videosIn(showDir))
            addEpisode(f, 1);
    }

    std::sort(result.begin(), result.end(), [](const Episode& a, const Episode& b) {
        return a.season != b.season ? a.season < b.season : a.episode < b.episode;
    });
    return result;
}
