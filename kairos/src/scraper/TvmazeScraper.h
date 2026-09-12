#pragma once
#include "IMetadataScraper.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>

// TVMaze scraper — free, unauthenticated public API (no key required).
// TV shows only: TVMaze doesn't catalogue movies, so searchMovies/fetchMovie
// are no-ops (empty/nullopt). ScraperManager only wires this scraper into
// the show-matching paths, never the movie ones.
class TvmazeScraper final : public IMetadataScraper {
public:
    TvmazeScraper();

    std::string sourceName() const override { return "tvmaze"; }

    std::vector<Show>    searchShows  (const std::string& title, int year = 0)                        override;
    std::optional<Show>  fetchShow    (const std::string& external_id, const std::string& lang = "") override;
    std::vector<Episode> fetchEpisodes(const std::string& external_id, const std::string& lang = "") override;

    // TVMaze has no movie catalogue — always returns empty/nullopt.
    std::vector<Movie>   searchMovies (const std::string& title, int year = 0)                        override;
    std::optional<Movie> fetchMovie   (const std::string& external_id, const std::string& lang = "") override;

private:
    httplib::Result get(const std::string& path);

    Show showFromJson(const nlohmann::json& j);

    httplib::Client client_;
};
