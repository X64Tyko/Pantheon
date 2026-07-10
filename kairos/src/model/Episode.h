#pragma once
#include <optional>
#include <string>
#include <cstdint>

struct Episode {
    std::string episode_id;
    std::string show_id;
    int         season      = 0;
    int         episode     = 0;
    std::string title;
    std::string file_path;
    int64_t     duration_ms = 0;

    std::string      overview;
    std::string      air_date;       // "YYYY-MM-DD"
    std::string      thumb;
    std::string      season_name;    // e.g. "Season 1", "Specials"; empty if unknown
    std::optional<int> absolute_index; // TVDB absolute episode number; null if not available

    std::string      tvdb_id;
    std::string      tmdb_id;
    std::string      imdb_id;

    // Watch state as reported by the source for its configured primary
    // account — transient, sync-time-only fields; never written to the
    // `episode` table. See Movie.h's identical fields for the full rationale.
    std::optional<bool>    src_watched;
    std::optional<int64_t> src_position_ms;
    std::optional<int64_t> src_watched_at;
};
