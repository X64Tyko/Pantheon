#pragma once
#include "IMetadataScraper.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>

// Trakt scraper. Trakt's public GET endpoints (search, show/movie/episode
// lookup) only need a registered app's Client ID sent as the trakt-api-key
// header — no OAuth user-token flow required for metadata scraping (passed
// as api_key in ScraperConfig, same slot TMDB/TVDB use for their keys).
//
// Trakt covers both TV and movies (unlike TVMaze), so this participates in
// both matchShow() and matchMovie() the same way TMDB/TVDB do.
class TraktScraper final : public IMetadataScraper
{
public:
	explicit TraktScraper(std::string client_id);

	std::string sourceName() const override { return "trakt"; }

	std::vector<Show> searchShows(const std::string& title, int year = 0) override;
	std::optional<Show> fetchShow(const std::string& external_id, const std::string& lang = "") override;
	std::vector<Episode> fetchEpisodes(const std::string& external_id, const std::string& lang = "") override;

	std::vector<Movie> searchMovies(const std::string& title, int year = 0) override;
	std::optional<Movie> fetchMovie(const std::string& external_id, const std::string& lang = "") override;

private:
	httplib::Result get(const std::string& path);

	Show showFromJson(const nlohmann::json& j);
	Movie movieFromJson(const nlohmann::json& j);

	// Fetches /shows|movies/{id}/translations (no language segment — Trakt
	// returns every language it has when one isn't specified) as alternate-
	// title candidates. `kind` is "shows" or "movies".
	std::vector<TitleTranslation> fetchTranslations(const std::string& kind, const std::string& external_id);

	std::string client_id_;
	httplib::Client client_;
};