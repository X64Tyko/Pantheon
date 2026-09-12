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
    std::vector<Show>     fetchShows(const std::string& external_lib_id)       override;
    std::vector<Movie>    fetchMovies(const std::string& external_lib_id)      override;
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

    std::vector<SourceUserInfo> listServerUsers() override;
    std::string                 lastUserDiscoveryError() const override { return last_user_discovery_error_; }

    std::vector<ExternalWatchState> fetchWatchState(const std::string& external_user_id) override;

    bool pushMetadata(const std::string& external_id,
                       const std::string& external_lib_id,
                       const std::string& item_type,
                       const WritebackFields& fields) override;

    std::optional<std::string> createRemoteList(const std::string& title, const std::string& kind,
                                                 const std::vector<IMediaSource::PushListItem>& items,
                                                 const std::string& external_lib_id) override;
    bool addRemoteListItems(const std::string& list_external_id, const std::string& kind,
                             const std::vector<IMediaSource::PushListItem>& items) override;
    bool removeRemoteListItems(const std::string& list_external_id, const std::string& kind,
                                const std::vector<IMediaSource::PushListItem>& items) override;

private:
    httplib::Result get(const std::string& path);

    // Plex's own server-instance id, needed for the "server://{id}/..."
    // library URIs createRemoteList/addRemoteListItems send — fetched once
    // (GET /identity) and cached; empty string means the fetch failed, which
    // callers treat as "push unsupported right now" rather than crashing.
    std::string machineIdentifier();
    std::optional<std::string> machine_identifier_;

    // Paginated GET across a playlist/collection-items endpoint (same
    // X-Plex-Container-Start/Size chunking as fetchShows/fetchMovies/
    // fetchEpisodes) — shared by browsePlaylistItems/browseCollectionItems/
    // fetchListItems so the pagination fix only has to exist once. nullopt
    // only when the *first* page request fails outright; a later page
    // failing mid-fetch just stops pagination and keeps whatever was
    // already collected.
    std::optional<std::vector<BrowseContentItem>> fetchAllBrowseItems(const std::string& basePath);

    std::string     source_id_;
    std::string     base_url_;
    std::string     token_;
    httplib::Client client_;
    std::string     last_user_discovery_error_;  // see lastUserDiscoveryError()
};
