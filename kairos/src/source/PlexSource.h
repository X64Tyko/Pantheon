#pragma once
#include "IMediaSource.h"
#include <httplib.h>
#include <string>

class PlexSource final : public IMediaSource {
public:
    PlexSource(const std::string& source_id,
               const std::string& base_url,
               const std::string& token);

    std::string sourceId()    const override { return source_id_; }
    std::string sourceType()  const override { return "plex"; }
    bool        isSupported() const override { return true; }

    // ── Library discovery ────────────────────────────────────────────────────
    std::vector<LibraryInfo> listAvailableLibraries() override;

    // ── Sync ─────────────────────────────────────────────────────────────────
    std::vector<Show>     fetchShows(const std::string& external_lib_id)     override;
    std::vector<Movie>    fetchMovies(const std::string& external_lib_id)    override;
    std::vector<Episode>  fetchEpisodes(const std::string& external_show_id) override;
    std::vector<Playlist> fetchPlaylists(const std::string& external_lib_id) override;

    // ── Browse ───────────────────────────────────────────────────────────────
    std::vector<BrowseListItem>    browsePlaylists()                                override;
    std::vector<BrowseContentItem> browsePlaylistItems(const std::string& id)       override;
    std::vector<BrowseListItem>    browseCollections(const std::string& ext_lib_id) override;
    std::vector<BrowseContentItem> browseCollectionItems(const std::string& id)     override;

    std::optional<std::vector<PlexListItem>>
        fetchListItems(const std::string& external_id, const std::string& plex_type) override;

    std::vector<Chapter> fetchIntroMarkers(const std::string& external_id) override;
    std::vector<Chapter> fetchChapters(const std::string& external_id)     override;

private:
    httplib::Result get(const std::string& path);

    std::string     source_id_;
    std::string     base_url_;
    std::string     token_;
    httplib::Client client_;
};
