#pragma once
#include "model/Episode.h"
#include "model/Movie.h"
#include "model/Playlist.h"
#include "model/Show.h"
#include "model/SourceConfig.h"
#include <cstdint>
#include <string>
#include <vector>

// A list entry returned by browsePlaylists / browseCollections.
struct BrowseListItem {
    std::string id;          // source-native key (Plex ratingKey, etc.)
    std::string title;
    int         item_count = 0;
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
};
