#include "LocalSource.h"
#include "model/Episode.h"
#include "model/Movie.h"
#include "model/Show.h"
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

// "Title (2023)" — year-in-parens suffix
const std::regex kYearRe(R"(\((\d{4})\)\s*$)");

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

// Parse a directory or file stem that may end with "(YYYY)".
// Returns { cleaned_title, optional<year> }.
std::pair<std::string, std::optional<int>> parseTitle(const std::string& name) {
    std::smatch m;
    if (std::regex_search(name, m, kYearRe)) {
        std::string title = name.substr(0, static_cast<size_t>(m.position()));
        while (!title.empty() && (title.back() == ' ' || title.back() == '.' || title.back() == '-'))
            title.pop_back();
        return {title, std::stoi(m[1].str())};
    }
    return {name, std::nullopt};
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

} // namespace

// ---------------------------------------------------------------------------

LocalSource::LocalSource(const std::string& source_id, const std::string& base_path)
    : source_id_(source_id), base_path_(base_path) {}

// ---------------------------------------------------------------------------
// Library discovery — each non-hidden immediate subdirectory of base_path_
// is offered as a separately-configurable library.  Type is auto-detected
// with a two-level heuristic:
//   • Any grandchild dir matches kSeasonDirRe  → "show"
//   • Any child dir has video files, or a video file sits at root level → "movie"
//   • Otherwise → "mixed"
// ---------------------------------------------------------------------------

static std::string guessLibraryType(const fs::path& dir) {
    std::error_code ec;
    bool hasVideos = false;
    for (const auto& child : fs::directory_iterator(dir, ec)) {
        if (isHidden(child.path())) continue;
        if (child.is_directory()) {
            // Season dir directly inside? → show library
            if (parseSeasonDir(child.path().filename().string()) >= 0) return "show";
            // Grandchild season dir? → show library
            for (const auto& gc : fs::directory_iterator(child.path(), ec)) {
                if (!gc.is_directory() || isHidden(gc.path())) continue;
                if (parseSeasonDir(gc.path().filename().string()) >= 0) return "show";
            }
            // Child dir contains videos → likely movie-per-folder layout
            if (!videosIn(child.path()).empty()) hasVideos = true;
        } else if (isVideo(child.path())) {
            hasVideos = true;
        }
    }
    return hasVideos ? "movie" : "mixed";
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

// ---------------------------------------------------------------------------
// Shows — each non-hidden top-level subdirectory is a show.
// ---------------------------------------------------------------------------

std::vector<Show> LocalSource::fetchShows(const std::string& external_lib_id) {
    std::error_code ec;
    if (!fs::is_directory(external_lib_id, ec)) return {};

    std::vector<Show> result;
    for (const auto& entry : fs::directory_iterator(external_lib_id, ec)) {
        if (!entry.is_directory() || isHidden(entry.path())) continue;
        auto [title, year] = parseTitle(entry.path().filename().string());
        Show show;
        show.show_id = entry.path().string(); // external key; SyncManager resolves
        show.title   = title;
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
            auto vfiles = videosIn(p);
            if (vfiles.empty()) continue;
            auto [title, year] = parseTitle(p.filename().string());
            Movie movie;
            movie.movie_id  = p.string();
            movie.title     = title;
            movie.file_path = vfiles.front().string();
            if (year) movie.year = year;
            result.push_back(std::move(movie));
        } else if (isVideo(p)) {
            auto [title, year] = parseTitle(p.stem().string());
            Movie movie;
            movie.movie_id  = p.string();
            movie.title     = title;
            movie.file_path = p.string();
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
        ep.episode_id = file.string();
        ep.show_id    = external_show_id;
        ep.file_path  = file.string();
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
