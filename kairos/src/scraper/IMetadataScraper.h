#pragma once
#include <optional>
#include <string>
#include <vector>
#include "../model/Episode.h"
#include "../model/Movie.h"
#include "../model/Show.h"

class IMetadataScraper {
public:
    virtual ~IMetadataScraper() = default;

    virtual std::string sourceName() const = 0;

    virtual std::vector<Show>    searchShows  (const std::string& title, int year = 0)                        = 0;
    virtual std::optional<Show>  fetchShow    (const std::string& external_id, const std::string& lang = "") = 0;
    virtual std::vector<Episode> fetchEpisodes(const std::string& external_id, const std::string& lang = "") = 0;

    virtual std::vector<Movie>   searchMovies (const std::string& title, int year = 0)                        = 0;
    virtual std::optional<Movie> fetchMovie   (const std::string& external_id, const std::string& lang = "") = 0;
};
