#pragma once
#include <cstdint>
#include <optional>
#include <string>

struct Movie {
    std::string    movie_id;
    std::string    title;
    std::string    content_rating;
    std::string    file_path;
    int64_t        duration_ms = 0;
    std::optional<int> year;
    std::string    release_date; // "YYYY-MM-DD" when known; empty if the source never provided one

    std::string    overview;
    std::string    tagline;
    std::string    studio;
    std::string    director;
    std::string    genres;          // JSON array string
    std::string    thumb;
    std::string    art;
    std::string    imdb_id;
    std::string    tmdb_id;
    std::optional<float> audience_rating;
    std::string    labels;       // JSON array: ["tag", ...]
    std::string    actors;       // JSON array of names
    std::string    countries;    // JSON array: ["United States", ...]
    std::string    collections;  // JSON array: ["Marvel", ...]

    // Watch state as reported by the source for its configured primary
    // account — transient, sync-time-only fields; never written to the
    // `movie` table. Consumed by SyncManager to seed watch_progress for
    // whichever local user the source is mapped to (media_source.synced_user_id).
    std::optional<bool>    src_watched;
    std::optional<int64_t> src_position_ms;
    std::optional<int64_t> src_watched_at; // epoch seconds, when the source provides one
    std::optional<int64_t> src_view_count; // real rewatch count when the source reports one (Plex viewCount / Jellyfin PlayCount)
};
