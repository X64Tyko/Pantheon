#pragma once
#include "IMetadataScraper.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>

// AniList scraper — free public GraphQL API (no key required), anime/manga
// only. Like AniDB, this is never queried automatically for a library;
// ScraperManager only queries it when a library's scraper priority order
// explicitly lists "anilist" (see the AniDB gating comment in matchShow/
// matchMovie — there's no separate "include_anilist" checkbox the way
// AniDB has one, priority-list membership is the only opt-in).
//
// AniList's schema has no per-episode data (titles/overviews/air dates) —
// only a total episode count on the show itself — so fetchEpisodes() always
// returns empty. It still participates in show/movie search and metadata.
class AnilistScraper final : public IMetadataScraper {
public:
    AnilistScraper();

    std::string sourceName() const override { return "anilist"; }

    std::vector<Show>    searchShows  (const std::string& title, int year = 0)                        override;
    std::optional<Show>  fetchShow    (const std::string& external_id, const std::string& lang = "") override;
    // AniList has no per-episode data — always returns {}.
    std::vector<Episode> fetchEpisodes(const std::string& external_id, const std::string& lang = "") override;

    std::vector<Movie>   searchMovies (const std::string& title, int year = 0)                        override;
    std::optional<Movie> fetchMovie   (const std::string& external_id, const std::string& lang = "") override;

private:
    // Posts a GraphQL query and returns the "data" object, or nullopt on any
    // transport/HTTP/GraphQL-error failure.
    std::optional<nlohmann::json> post(const std::string& query, const nlohmann::json& variables);

    Show  showFromJson (const nlohmann::json& j);
    Movie movieFromJson(const nlohmann::json& j);

    httplib::Client client_;
};
