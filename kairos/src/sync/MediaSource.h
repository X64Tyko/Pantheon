#pragma once
#include "model/Episode.h"
#include "model/Movie.h"
#include "model/Playlist.h"
#include "model/Show.h"
#include "model/SourceConfig.h"
#include <string>
#include <vector>

class MediaSource {
public:
    virtual ~MediaSource() = default;

    virtual std::string sourceId()    const = 0;
    virtual std::string sourceType()  const = 0;
    virtual bool        isSupported() const = 0;

    // Admin: present available libraries on this server for user selection
    virtual std::vector<LibraryInfo> listAvailableLibraries() = 0;

    // Sync: only called for libraries the user has explicitly added
    virtual std::vector<Show>     fetchShows(const std::string& external_lib_id)      = 0;
    virtual std::vector<Movie>    fetchMovies(const std::string& external_lib_id)     = 0;
    virtual std::vector<Episode>  fetchEpisodes(const std::string& external_show_id)  = 0;
    virtual std::vector<Playlist> fetchPlaylists(const std::string& external_lib_id)  = 0;
};
