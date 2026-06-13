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
    std::string library_type; // "show" | "movie" | "mixed"
    bool        enabled = true;
};

// Returned by MediaSource::listAvailableLibraries() — live from the server
struct LibraryInfo {
    std::string external_lib_id;
    std::string name;
    std::string type; // "show" | "movie" | "mixed"
};
