#pragma once
#include "IMetadataScraper.h"
#include <chrono>
#include <httplib.h>
#include <mutex>
#include <string>
#include <vector>

// AniDB HTTP API scraper.
// Searches the public title dump (cached 24 h) and fetches anime detail XML
// from the AniDB HTTP API (rate-limited to 1 req/2 s).
//
// client_name must be a client registered at https://anidb.net/software/add
// (passed as api_key in ScraperConfig).
class AnidbScraper final : public IMetadataScraper {
public:
    explicit AnidbScraper(std::string client_name);

    std::string sourceName() const override { return "anidb"; }

    std::vector<Show>    searchShows  (const std::string& title, int year = 0) override;
    std::optional<Show>  fetchShow    (const std::string& external_id)         override;
    std::vector<Episode> fetchEpisodes(const std::string& external_id)         override;

    std::vector<Movie>   searchMovies (const std::string& title, int year = 0) override;
    std::optional<Movie> fetchMovie   (const std::string& external_id)         override;

private:
    struct TitleMatch { std::string aid, title; double score; };

    bool                     ensureTitleDump();
    std::vector<TitleMatch>  searchTitleDump(const std::string& query) const;
    std::string              fetchAnimeXml(const std::string& aid);
    void                     rateLimitWait();

    Show                 showFromXml   (const std::string& xml, const std::string& aid);
    Movie                movieFromXml  (const std::string& xml, const std::string& aid);
    std::vector<Episode> episodesFromXml(const std::string& xml, const std::string& show_id);

    std::string     client_name_;
    httplib::Client api_client_;   // https://api.anidb.net:9001
    httplib::Client dump_client_;  // http://anidb.net (title dump)

    std::mutex                            rate_mu_;
    std::chrono::steady_clock::time_point last_api_call_;

    static constexpr const char* kTitlesXml = "/tmp/kairos-anidb-titles.xml";
    static constexpr const char* kTitlesGz  = "/tmp/kairos-anidb-titles.xml.gz";
    static constexpr const char* kImgBase   = "https://cdn-us.anidb.net/images/main/";
};
