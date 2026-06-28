#pragma once
#include "IMetadataScraper.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <string>

class TvdbScraper final : public IMetadataScraper {
public:
    TvdbScraper(std::string api_key, std::string language = "eng", std::string pin = "");

    std::string sourceName() const override { return "tvdb"; }

    std::vector<Show>    searchShows  (const std::string& title, int year = 0) override;
    std::optional<Show>  fetchShow    (const std::string& external_id)         override;
    std::vector<Episode> fetchEpisodes(const std::string& external_id)         override;

    std::vector<Movie>   searchMovies (const std::string& title, int year = 0) override;
    std::optional<Movie> fetchMovie   (const std::string& external_id)         override;

private:
    bool        ensureToken();
    httplib::Result get(const std::string& path);

    Show    showFromJson   (const nlohmann::json& j);
    Movie   movieFromJson  (const nlohmann::json& j);
    Episode episodeFromJson(const nlohmann::json& j, const std::string& show_id);

    std::string     api_key_;
    std::string     language_;
    std::string     pin_;
    std::string     token_;
    int64_t         token_expiry_{0};   // Unix timestamp; 0 = not obtained
    std::mutex      token_mu_;
    httplib::Client client_;
};
