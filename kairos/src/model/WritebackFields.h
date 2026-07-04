#pragma once
#include <string>

// Text metadata pushed back into a source library by IMediaSource::pushMetadata.
// Deliberately text-only for v1 — artwork requires a separate binary-upload
// mechanism on both Plex and Jellyfin and is out of scope for this pass.
// Empty string means "don't touch this field" (not "clear it").
struct WritebackFields {
    std::string title;
    std::string overview;
    std::string genres;         // JSON array string, same encoding as Show/Movie.genres
    std::string content_rating;
    std::string studio;
    std::string network;        // show-only
    std::string director;       // movie-only
    std::string tagline;        // movie-only
    std::string actors;         // JSON array string
    std::string countries;      // JSON array string
    std::string collections;    // JSON array string
    std::string release_date;   // movie: release_date; show: originally_available_at
};
