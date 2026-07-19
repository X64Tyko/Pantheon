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
//
// JellyfinBaseSource sends genres/actors/director/writer (see its
// pushMetadata — it already fetches-and-replaces the whole item object, so
// Genres is a plain array-replace and People/Director/Writer are a
// Type-scoped replace that leaves every other credit type untouched).
// countries/collections remain unsent there — no user-reported need yet.
//
// PlexSource sends genres/director/writer/collections (movies+shows) and
// countries (movies only, matching Plex's own ShowEditMixins not having a
// Country mixin) via a fresh-fetch-then-diff against Plex's add/remove tag
// directives (`tag[n].tag.tag=value` to add/set an index, `tag[].tag.tag-`
// to remove, no single "replace" directive) — verified against
// python-plexapi's actual implementation, not guessed. `actors` is NOT sent
// by either source: Jellyfin's is a documented gap (no user-reported need
// yet, same reasoning as countries/collections), but Plex's is structural —
// even python-plexapi, the reference client library, has no Role/Actor edit
// mixin at all, so there's no known-safe mechanism to verify against, let
// alone implement.
//
// thumb/art: unset optional means "don't touch"; set means "replace with
// these bytes" (images have no partial-clear concept, unlike the text
// fields' empty-string convention).
struct WritebackFields {
    std::string title;
    std::string original_title;
    std::string overview;
    std::string genres;         // JSON array string, same encoding as Show/Movie.genres
    std::string content_rating;
    std::string studio;
    std::string network;        // show-only
    std::string director;       // movie-only
    std::string writer;         // movie-only
    std::string tagline;        // movie-only
    std::string actors;         // JSON array string
    std::string countries;      // JSON array string
    std::string collections;    // JSON array string
    std::string release_date;   // movie: release_date; show: originally_available_at
    std::optional<WritebackImage> thumb; // poster
    std::optional<WritebackImage> art;   // backdrop/fanart
};
