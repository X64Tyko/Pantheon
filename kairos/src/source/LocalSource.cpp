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

// A standalone 4-digit release year (1900-2099), whether bare ("Title 2023 ...")
// or parenthesised ("Title (2023)"). Word-bounded so it can't fire inside a
// resolution/codec tag like "2160p" or "x264".
const std::regex kYearTokenRe(R"(\b(19\d{2}|20\d{2})\b)");

// Scene-release quality/source/codec/audio/edition tags. The *earliest* match
// marks where junk starts when no year token is present to anchor the cut —
// e.g. "The.Toxic.Avenger.UNRATED.BluRay.x264-GROUP" has no year at all.
const std::regex kJunkTagRe(
    R"(\b(2160p|1080p|720p|480p|360p|4k|8k|uhd|hdr10?|dolby ?vision|)"
    R"(bluray|blu-ray|bdrip|brrip|bdremux|remux|webrip|web-?dl|webdl|hdtv|pdtv|)"
    R"(dvdrip|dvdscr|dvd5|dvd9|hdcam|camrip|cam|telesync|hdrip|)"
    R"(x264|x265|h ?264|h ?265|hevc|avc1?|xvid|divx|)"
    R"(aac(?:2 ?0)?|ac3|eac3|dts(?:-?hd)?|ddp?5 ?1|ddp?7 ?1|atmos|truehd|flac|)"
    R"(proper|repack|internal|limited|extended(?:\s?cut)?|unrated|uncut|)"
    R"(director'?s cut|theatrical(?:\s?cut)?|remastered|imax|10bit|8bit|)"
    R"(yify|yts(?:\.mx)?|rarbg)\b)",
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
std::pair<std::string, std::optional<int>> parseTitle(const std::string& raw) {
    // Scene releases use '.'/'_' as word separators; normalise to spaces so
    // the regexes below see real word boundaries ("The.Thing" -> "The Thing").
    // Collapse runs of spaces this creates (also tidies "Mr. Robot"-style names).
    std::string name;
    name.reserve(raw.size());
    bool prev_space = false;
    for (char c : raw) {
        if (c == '.' || c == '_') c = ' ';
        if (c == ' ') {
            if (prev_space) continue;
            prev_space = true;
        } else {
            prev_space = false;
        }
        name += c;
    }

    auto trimmed = [](std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '.' ||
                               s.back() == '-' || s.back() == '('))
            s.pop_back();
        size_t start = s.find_first_not_of(' ');
        return start == std::string::npos ? std::string() : s.substr(start);
    };

    // Look for a year token, skipping one that sits at the very start of the
    // name — that's almost always the title itself ("1917", "2001 A Space
    // Odyssey"), not a release-year marker, so keep scanning for a later one.
    std::optional<int> year;
    size_t cut = std::string::npos;
    for (auto it = std::sregex_iterator(name.begin(), name.end(), kYearTokenRe);
         it != std::sregex_iterator(); ++it) {
        size_t pos = static_cast<size_t>(it->position());
        if (trimmed(name.substr(0, pos)).empty()) continue;
        year = std::stoi((*it)[1].str());
        cut = pos;
        break;
    }

    // No year marker found: fall back to the first quality/source/edition tag.
    if (cut == std::string::npos) {
        std::smatch jm;
        if (std::regex_search(name, jm, kJunkTagRe) && jm.position() > 0)
            cut = static_cast<size_t>(jm.position());
    }

    std::string title = trimmed(cut == std::string::npos ? name : name.substr(0, cut));
    return {title, year};
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
        show.show_id     = entry.path().string(); // external key; SyncManager resolves
        show.title       = title;
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
            auto vfiles = videosIn(p);
            if (vfiles.empty()) continue;
            auto [title, year] = parseTitle(p.filename().string());
            Movie movie;
            movie.movie_id    = p.string();
            movie.title       = title;
            movie.file_path   = vfiles.front().string();
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
            movie.movie_id    = p.string();
            movie.title       = title;
            movie.file_path   = p.string();
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
