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

    std::vector<Show>    searchShows  (const std::string& title, int year = 0)                        override;
    std::optional<Show>  fetchShow    (const std::string& external_id, const std::string& lang = "") override;
    std::vector<Episode> fetchEpisodes(const std::string& external_id, const std::string& lang = "") override;

    std::vector<Movie>   searchMovies (const std::string& title, int year = 0)                        override;
    std::optional<Movie> fetchMovie   (const std::string& external_id, const std::string& lang = "") override;

private:
    struct TitleMatch { std::string aid, title; double score; };

    bool                     ensureTitleDump();
    std::vector<TitleMatch>  searchTitleDump(const std::string& query) const;
    void                     rateLimitWait();

public:
    // Returns the CDN poster URL for an AID, or empty if unavailable.
    // Caches the anime XML to disk so repeated calls are instant.
    std::string posterUrl(const std::string& aid);

    // Same 2.1 s gate as the HTTP API, but its own independent clock — image
    // fetches hit cdn.anidb.net, a different host from api.anidb.net:9001,
    // so a burst of one must never eat into (or be blocked by) the other's
    // budget. ContentService::proxyImage calls this before fetching from
    // cdn.anidb.net — that path used to be completely unthrottled, which is
    // what was actually tripping cdn.anidb.net's own hotlink/abuse
    // protection (seen as 502s) after the API-side lookups had already
    // burned through a minute-plus of 2.1 s waits.
    void rateLimitImageWait();

private:
    std::string              fetchAnimeXml(const std::string& aid);

    Show                 showFromXml   (const std::string& xml, const std::string& aid);
    Movie                movieFromXml  (const std::string& xml, const std::string& aid);
    std::vector<Episode> episodesFromXml(const std::string& xml, const std::string& show_id);

    std::string     client_name_;
    httplib::Client api_client_;   // https://api.anidb.net:9001
    httplib::Client dump_client_;  // http://anidb.net (title dump)

    std::mutex                            rate_mu_;
    std::chrono::steady_clock::time_point last_api_call_;

    std::mutex                            image_rate_mu_;
    std::chrono::steady_clock::time_point last_image_call_;

    static constexpr const char* kTitlesXml  = "/tmp/kairos-anidb-titles.xml";
    static constexpr const char* kTitlesGz   = "/tmp/kairos-anidb-titles.xml.gz";
    static constexpr const char* kXmlCacheDir = "anidb-xml-cache";
    // No region-specific "-us"/"-eu" subdomain — cdn-us.anidb.net (the
    // previous value here) doesn't appear in any current AniDB documentation
    // or examples; the bare cdn.anidb.net is what every current source
    // actually uses. This is exactly the kind of hardcoded-hostname rot a
    // sibling project (ShokoServer) hit too — AniDB has moved image hosts
    // before with no redirect from the old one.
    static constexpr const char* kImgBase    = "https://cdn.anidb.net/images/main/";
};
