#pragma once
#include "IMetadataScraper.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <unordered_map>

// Wikidata scraper — free public API (no key required). Structured
// metadata (title, release/air dates, genres, network/studio, country, an
// MPA rating for movies) for anything with a Wikidata item, which is a much
// broader net than TMDB/TVDB/TVMaze/Trakt cover — obscure/regional/web-only
// titles ("Dr. Horrible's Sing-Along Blog") that are thin or absent in the
// mainstream metadata DBs usually still have one.
//
// Deliberately NOT a wikitext/infobox scraper — Wikidata's structured claim
// graph (stable numbered properties: P31 "instance of", P577 "publication
// date", P136 "genre", ...) is used instead of parsing Wikipedia article
// markup, which is inconsistent per-article and breaks on reformatting.
//
// Two-tier fields, same shape as TmdbScraper's is_detail split: searchShows/
// searchMovies return thin results (title/year/date/short description, no
// genres/network/country) from just the search + one batched entity fetch;
// fetchShow/fetchMovie do the extra work — batch-resolving genre/network/
// country Q-ID references to labels, and fetching a real plain-text summary
// (+ poster thumbnail) from the linked English Wikipedia article via the
// entity's enwiki sitelink (which Wikidata already resolves, so this is a
// direct lookup, not a second fuzzy title match). Wikidata's own P18 image
// claim is left unused as a poster source — verified live that it's almost
// always empty for shows/movies, since Commons only hosts freely-licensed
// media and official posters are copyrighted; Wikipedia's summary API host
// a fair-use thumbnail instead, which is what's actually used here.
//
// No per-episode data: Wikidata doesn't reliably carry episode-level items
// for most shows, so fetchEpisodes() always returns {}.
class WikidataScraper final : public IMetadataScraper {
public:
    WikidataScraper();

    std::string sourceName() const override { return "wikidata"; }

    std::vector<Show>    searchShows  (const std::string& title, int year = 0)                        override;
    std::optional<Show>  fetchShow    (const std::string& external_id, const std::string& lang = "") override;
    std::vector<Episode> fetchEpisodes(const std::string& external_id, const std::string& lang = "") override;

    std::vector<Movie>   searchMovies (const std::string& title, int year = 0)                        override;
    std::optional<Movie> fetchMovie   (const std::string& external_id, const std::string& lang = "") override;

private:
    struct Candidate { std::string qid; nlohmann::json entity; };

    // Fuzzy-searches for `title`, then filters down to results whose P31
    // ("instance of") claim matches one of `allowed_p31` — wbsearchentities
    // has no type filter of its own; a plain "Alien" search also returns
    // video games, a fictional species, and unrelated senses of the word,
    // so this is what actually narrows it down to film/show entities.
    std::vector<Candidate> searchAndFilter(const std::string& title, const std::set<std::string>& allowed_p31);

    // Batch-resolves Q-IDs to English labels — genre/network/studio/
    // country/rating claim *values* are themselves Q-ID references, not
    // plain strings. Chunked at 50 ids/call (Wikidata's per-request max).
    std::unordered_map<std::string, std::string> resolveLabels(const std::set<std::string>& qids);

    // Best-effort plain-text extract (+ thumbnail URL) from the English
    // Wikipedia article named by the entity's enwiki sitelink. Empty
    // extract/thumbnail on any failure or absent sitelink.
    std::string wikipediaOverview(const std::string& enwiki_title, std::string* thumbnail_out);

    nlohmann::json fetchEntity(const std::string& qid);

    Show  buildShow (const nlohmann::json& entity, bool full_detail);
    Movie buildMovie(const nlohmann::json& entity, bool full_detail);

    // Wikidata's action API applies a short-window burst rate limit that a
    // normal library-matching pass trips easily (every title does 2+ calls
    // here) — live-verified: the 3rd lookup in a row within a couple of
    // seconds got a 429. Retries on 429 with a short backoff (honoring
    // Retry-After if present) before giving up, same defensive posture
    // AniDB's scraper already takes for its own documented rate limit.
    httplib::Result getWithRetry(httplib::Client& client, const std::string& path);

    httplib::Client wikidata_client_;
    httplib::Client wikipedia_client_;
};
