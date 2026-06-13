#pragma once
#include "MediaSource.h"
#include <httplib.h>
#include <string>

class PlexSource final : public MediaSource {
public:
    PlexSource(const std::string& source_id,
               const std::string& base_url,
               const std::string& token);

    std::string sourceId()    const override { return source_id_; }
    std::string sourceType()  const override { return "plex"; }
    bool        isSupported() const override { return true; }

    std::vector<LibraryInfo> listAvailableLibraries()                            override;
    std::vector<Show>        fetchShows(const std::string& external_lib_id)      override;
    std::vector<Movie>       fetchMovies(const std::string& external_lib_id)     override;
    std::vector<Episode>     fetchEpisodes(const std::string& external_show_id)  override;
    std::vector<Playlist>    fetchPlaylists(const std::string& external_lib_id)  override;

private:
    httplib::Result get(const std::string& path);

    std::string     source_id_;
    std::string     base_url_;
    std::string     token_;
    httplib::Client client_;
};
