#pragma once
#include "IMediaSource.h"
#include <string>

// Local filesystem source — not yet implemented.
// Visible in the UI but not selectable (isSupported = false).
// base_path is the root directory of the local library.
class LocalSource final : public IMediaSource {
public:
    LocalSource(const std::string& source_id, const std::string& base_path)
        : source_id_(source_id), base_path_(base_path) {}

    std::string sourceId()    const override { return source_id_; }
    std::string sourceType()  const override { return "local"; }
    bool        isSupported() const override { return false; }

    std::vector<LibraryInfo>       listAvailableLibraries()                     override { return {}; }
    std::vector<Show>              fetchShows(const std::string&)                override { return {}; }
    std::vector<Movie>             fetchMovies(const std::string&)               override { return {}; }
    std::vector<Episode>           fetchEpisodes(const std::string&)             override { return {}; }
    std::vector<Playlist>          fetchPlaylists(const std::string&)            override { return {}; }
    std::vector<BrowseListItem>    browsePlaylists()                             override { return {}; }
    std::vector<BrowseContentItem> browsePlaylistItems(const std::string&)       override { return {}; }
    std::vector<BrowseListItem>    browseCollections(const std::string&)         override { return {}; }
    std::vector<BrowseContentItem> browseCollectionItems(const std::string&)     override { return {}; }

private:
    std::string source_id_;
    std::string base_path_;
};
