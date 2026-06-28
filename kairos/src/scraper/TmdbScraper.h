#pragma once
#include "IMetadataScraper.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>

class TmdbScraper final : public IMetadataScraper {
public:
    TmdbScraper(std::string api_key, std::string language = "en");

    std::string sourceName() const override { return "tmdb"; }

    std::vector<Show>    searchShows  (const std::string& title, int year = 0) override;
    std::optional<Show>  fetchShow    (const std::string& external_id)         override;
    std::vector<Episode> fetchEpisodes(const std::string& external_id)         override;

    std::vector<Movie>   searchMovies (const std::string& title, int year = 0) override;
    std::optional<Movie> fetchMovie   (const std::string& external_id)         override;

    // Full poster URL for a TMDB poster_path (may be empty string → return "").
    static std::string posterUrl(const std::string& path);

private:
    httplib::Result get(const std::string& path);

    Show    showFromJson   (const nlohmann::json& j, bool is_detail = false);
    Movie   movieFromJson  (const nlohmann::json& j, bool is_detail = false);
    Episode episodeFromJson(const nlohmann::json& j, const std::string& show_id);

    std::string   api_key_;
    std::string   language_;
    httplib::Client client_;
};
