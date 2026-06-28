#pragma once
#include "IMediaSource.h"
#include <httplib.h>
#include <string>

// Shared base for JellyfinSource and EmbySource.
// Both use the same REST API; the only runtime differences are the auth header
// name and the string returned by sourceType(). Not for direct instantiation.
class JellyfinBaseSource : public IMediaSource {
public:
    bool isSupported() const override { return true; }

    std::vector<LibraryInfo>       listAvailableLibraries()                           override;
    std::vector<Show>              fetchShows(const std::string& external_lib_id)     override;
    std::vector<Movie>             fetchMovies(const std::string& external_lib_id)    override;
    std::vector<Episode>           fetchEpisodes(const std::string& external_show_id) override;
    std::vector<Playlist>          fetchPlaylists(const std::string& external_lib_id) override;

    std::vector<BrowseListItem>    browsePlaylists()                                   override;
    std::vector<BrowseContentItem> browsePlaylistItems(const std::string& id)         override;
    std::vector<BrowseListItem>    browseCollections(const std::string& ext_lib_id)   override;
    std::vector<BrowseContentItem> browseCollectionItems(const std::string& id)       override;

protected:
    JellyfinBaseSource(const std::string& source_id,
                       const std::string& base_url,
                       const std::string& token,
                       const std::string& user_id,
                       const std::string& auth_header);

    httplib::Result get(const std::string& path);

    std::string     source_id_;
    std::string     base_url_;
    std::string     token_;
    std::string     user_id_;
    std::string     auth_header_;  // "X-MediaBrowser-Token" or "X-Emby-Token"
    httplib::Client client_;
};
