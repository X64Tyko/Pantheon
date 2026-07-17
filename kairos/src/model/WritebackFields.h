#pragma once
#include <optional>
#include <string>

// Raw bytes for a poster/art writeback, already resolved from whatever form
// Show/Movie.thumb|art was in (CDN URL, source-relative path, or a "local:"
// on-disk file — see ContentService::fetchImageBytes) — IMediaSource
// implementations just upload bytes, they don't resolve paths themselves.
struct WritebackImage {
    std::string bytes;
    std::string content_type;   // e.g. "image/jpeg" — sent as-is as the upload's Content-Type
};

// Metadata pushed back into a source library by IMediaSource::pushMetadata.
// Text fields: empty string means "don't touch this field" (not "clear it").
// Array-valued text fields (genres/actors/countries/collections/director/
// network) are populated here but deliberately NOT sent by PlexSource —
// Plex and Jellyfin both model them as multi-value tag/People lists rather
// than a scalar, and the add/replace-vs-merge semantics need live
// verification against a real server before it's safe to guess at (a wrong
// shape risks corrupting existing tags, not just failing to apply).
// thumb/art: unset optional means "don't touch"; set means "replace with
// these bytes" (images have no partial-clear concept, unlike the text
// fields' empty-string convention).
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
    std::optional<WritebackImage> thumb; // poster
    std::optional<WritebackImage> art;   // backdrop/fanart
};
