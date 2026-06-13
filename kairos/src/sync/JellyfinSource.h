#pragma once
#include "MediaSource.h"
#include <string>

class JellyfinSource final : public MediaSource {
public:
    JellyfinSource(const std::string& source_id,
                   const std::string& base_url,
                   const std::string& token,
                   const std::string& user_id)
        : source_id_(source_id), base_url_(base_url)
        , token_(token), user_id_(user_id) {}

    std::string sourceId()    const override { return source_id_; }
    std::string sourceType()  const override { return "jellyfin"; }
    bool        isSupported() const override { return false; }

    std::vector<LibraryInfo> listAvailableLibraries()              override { return {}; }
    std::vector<Show>        fetchShows(const std::string&)        override { return {}; }
    std::vector<Movie>       fetchMovies(const std::string&)       override { return {}; }
    std::vector<Episode>     fetchEpisodes(const std::string&)     override { return {}; }
    std::vector<Playlist>    fetchPlaylists(const std::string&)    override { return {}; }

private:
    std::string source_id_;
    std::string base_url_;
    std::string token_;
    std::string user_id_;
};
