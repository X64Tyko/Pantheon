#pragma once
#include "model/Chapter.h"
#include "model/Episode.h"
#include "model/Movie.h"
#include "model/Playlist.h"
#include "model/Show.h"
#include "model/SourceConfig.h"
#include "model/WritebackFields.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A list entry returned by browsePlaylists / browseCollections.
struct BrowseListItem {
    std::string id;          // source-native key (Plex ratingKey, etc.)
    std::string title;
    int         item_count = 0;
};

// Minimal item for plex-sync: external_id + type only.
struct PlexListItem {
    std::string item_type;   // "movie" | "episode"
    std::string external_id; // source-native ratingKey
};

// A content item returned by browsePlaylistItems / browseCollectionItems.
// kairos_id resolution is left to the caller (Router → DB lookup).
struct BrowseContentItem {
    std::string external_id;  // source-native key — caller resolves to kairos_id
    std::string item_type;    // "movie" | "episode"
    std::string title;
    int64_t     duration_ms = 0;
    // Episode-only fields (empty/−1 for movies).
    std::string show_title;
    int         season  = -1;
    int         episode = -1;
};

class IMediaSource {
public:
    virtual ~IMediaSource() = default;

    virtual std::string sourceId()    const = 0;
    virtual std::string sourceType()  const = 0;
    virtual bool        isSupported() const = 0;

    // ── Library discovery ────────────────────────────────────────────────────
    virtual std::vector<LibraryInfo> listAvailableLibraries() = 0;

    // ── Sync ─────────────────────────────────────────────────────────────────
    // library_type is the library's configured type ("show"/"movie"/"mixed") —
    // only LocalSource uses it (to decide, per subdirectory, whether an item
    // belongs in a "mixed" library's show or movie table); every other source
    // gets unambiguous content typing from the server itself and ignores it.
    virtual std::vector<Show>     fetchShows(const std::string& external_lib_id)     = 0;
    virtual std::vector<Movie>    fetchMovies(const std::string& external_lib_id)    = 0;
    // fetchEpisodes is called concurrently — implementations must be thread-safe.
    virtual std::vector<Episode>  fetchEpisodes(const std::string& external_show_id) = 0;
    virtual std::vector<Playlist> fetchPlaylists(const std::string& external_lib_id) = 0;

    // ── Browse (live queries, not synced data) ────────────────────────────────
    virtual std::vector<BrowseListItem>    browsePlaylists()                                   = 0;
    virtual std::vector<BrowseContentItem> browsePlaylistItems(const std::string& id)          = 0;
    virtual std::vector<BrowseListItem>    browseCollections(const std::string& ext_lib_id)    = 0;
    virtual std::vector<BrowseContentItem> browseCollectionItems(const std::string& id)        = 0;

    // ── Plex sync helper ─────────────────────────────────────────────────────
    // Fetches items from a Plex playlist or collection by external_id + plex_type.
    // Returns nullopt on network/parse error; empty vector = success but no items.
    virtual std::optional<std::vector<PlexListItem>>
        fetchListItems(const std::string& /*external_id*/, const std::string& /*plex_type*/) {
        return std::vector<PlexListItem>{};
    }

    // ── Chapter data ──────────────────────────────────────────────────────────
    // Typed intro/credits markers for a single media item (external_id).
    // source="plex_intro"; chapter_type is "intro" or "credits".
    virtual std::vector<Chapter> fetchIntroMarkers(const std::string& /*external_id*/) {
        return {};
    }
    // Generic chapter points embedded in source metadata (external_id).
    // source="plex_chapters"; chapter_type is "unclassified".
    virtual std::vector<Chapter> fetchChapters(const std::string& /*external_id*/) {
        return {};
    }

    // ── Metadata writeback ───────────────────────────────────────────────────
    // Pushes corrected metadata back into the source library for one item.
    // Default: unsupported. Callers must check the return value — false means
    // either this source type doesn't implement writeback yet, or the push
    // itself failed; either way nothing was written.
    // external_lib_id: the source's library/section id (Plex "section"),
    // needed by sources whose write endpoint is scoped per-library.
    virtual bool pushMetadata(const std::string& /*external_id*/,
                               const std::string& /*external_lib_id*/,
                               const std::string& /*item_type*/,
                               const WritebackFields& /*fields*/) {
        return false;
    }
};
