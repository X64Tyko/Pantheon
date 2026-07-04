#pragma once
#include <string>

struct MediaSourceConfig {
    std::string source_id;
    std::string source_type; // "plex" | "jellyfin" | "emby" | "local"
    std::string display_name;
    std::string base_url;    // empty for local sources
    bool        enabled = true;
};

struct MediaLibraryConfig {
    std::string library_id;
    std::string source_id;
    std::string external_lib_id;
    std::string display_name;
    std::string library_type;         // "show" | "movie" | "mixed" | "music" | "photo"
    std::string preferred_scraper;    // "" | "tmdb" | "tvdb" | "anidb"
    std::string preferred_language;   // ISO 639-1 e.g. "en", "ja"; empty = scraper default
    bool        enabled = true;
    // AniDB is anime-only — never queried automatically for a library unless
    // explicitly opted in here (or preferred_scraper is set to "anidb" outright).
    bool        include_anidb = false;
};

// Returned by MediaSource::listAvailableLibraries() — live from the server
struct LibraryInfo {
    std::string external_lib_id;
    std::string name;
    std::string type; // "show" | "movie" | "mixed" | "music" | "photo"
};
