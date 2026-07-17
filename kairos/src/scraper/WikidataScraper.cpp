#include "WikidataScraper.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;

static constexpr const char* kWikidataBase  = "https://www.wikidata.org";
static constexpr const char* kWikipediaBase = "https://en.wikipedia.org";

// "instance of" (P31) values that count as a TV show / movie respectively.
// wbsearchentities has no type filter, so a bare "Alien" search also returns
// video games, a fictional species, and unrelated senses of the word — these
// allowlists (live-verified against real Wikidata entities while building
// this) are what actually narrows results down to relevant media.
static const std::set<std::string> kShowTypes  = {"Q5398426", "Q526877", "Q15416"};
// Q5398426 television series, Q526877 web series, Q15416 television program
static const std::set<std::string> kMovieTypes = {"Q11424", "Q506240", "Q29168811"};
// Q11424 film, Q506240 television film, Q29168811 animated feature film

// Wikimedia's API etiquette policy requires a descriptive User-Agent
// identifying the application on every request; requests without one are
// aggressively rate-limited (observed live: a 429 after only ~2-3 calls in
// quick succession with no User-Agent set, vs. none after adding this).
static constexpr const char* kUserAgent = "Pantheon-Kairos/1.0 (media metadata scraper; self-hosted)";

WikidataScraper::WikidataScraper()
    : wikidata_client_(kWikidataBase)
    , wikipedia_client_(kWikipediaBase)
{
    wikidata_client_.set_connection_timeout(10);
    wikidata_client_.set_read_timeout(20);
    wikidata_client_.set_default_headers({{"User-Agent", kUserAgent}});
    wikipedia_client_.set_default_headers({{"User-Agent", kUserAgent}});
    wikipedia_client_.set_connection_timeout(10);
    wikipedia_client_.set_read_timeout(20);
}

httplib::Result WikidataScraper::getWithRetry(httplib::Client& client, const std::string& path) {
    constexpr int kMaxAttempts = 3;
    httplib::Result res;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        res = client.Get(path.c_str());
        if (!res || res->status != 429) return res;

        int wait_ms = 1000 * (attempt + 1); // 1s, 2s, 3s
        if (auto it = res->headers.find("Retry-After"); it != res->headers.end()) {
            try { wait_ms = std::max(wait_ms, std::stoi(it->second) * 1000); } catch (...) {}
        }
        std::cerr << "[wikidata] 429 rate-limited, retrying in " << wait_ms << "ms (attempt "
                  << (attempt + 1) << "/" << kMaxAttempts << ")\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
    return res;
    wikipedia_client_.set_connection_timeout(10);
    wikipedia_client_.set_read_timeout(20);
}

// ── JSON helpers ─────────────────────────────────────────────────────────────

// See TmdbScraper.cpp/TvdbScraper.cpp — httplib::detail::encode_url() doesn't
// escape a literal '%'. Pre-escape it ourselves before encoding.
static std::string encodeQuery(const std::string& s) {
    std::string escaped;
    escaped.reserve(s.size());
    for (char c : s) {
        if (c == '%') escaped += "%25";
        else escaped += c;
    }
    return httplib::detail::encode_url(escaped);
}

static std::string labelOf(const json& entity, const std::string& lang_key) {
    if (entity.contains("labels") && entity["labels"].contains(lang_key))
        return entity["labels"][lang_key].value("value", "");
    return "";
}

static std::string descriptionOf(const json& entity, const std::string& lang_key) {
    if (entity.contains("descriptions") && entity["descriptions"].contains(lang_key))
        return entity["descriptions"][lang_key].value("value", "");
    return "";
}

// Q-ID references (genre/network/studio/country/rating claims are all
// entity-type — the claim's *value* is itself a Wikidata item, not a plain
// string, so these need a second lookup to get a human-readable label).
static std::vector<std::string> claimEntityIds(const json& entity, const std::string& pid) {
    std::vector<std::string> out;
    if (!entity.contains("claims") || !entity["claims"].contains(pid)) return out;
    for (const auto& claim : entity["claims"][pid]) {
        auto mainsnak = claim.value("mainsnak", json::object());
        if (mainsnak.value("snaktype", "") != "value") continue;
        auto dv = mainsnak.value("datavalue", json::object());
        if (dv.value("type", "") != "wikibase-entityid") continue;
        std::string id = dv.value("value", json::object()).value("id", "");
        if (!id.empty()) out.push_back(id);
    }
    return out;
}

static bool hasAnyType(const json& entity, const std::set<std::string>& allowed) {
    for (const auto& t : claimEntityIds(entity, "P31"))
        if (allowed.count(t)) return true;
    return false;
}

struct WikidataDate { std::string iso; int year = 0; };

// A property can carry multiple time claims (e.g. regional release dates) —
// takes the earliest. Wikidata time values look like "+2008-07-15T00:00:00Z";
// the leading '+' has to be stripped before the year substring is parsed.
static std::optional<WikidataDate> claimEarliestDate(const json& entity, const std::string& pid) {
    if (!entity.contains("claims") || !entity["claims"].contains(pid)) return std::nullopt;
    std::optional<WikidataDate> best;
    for (const auto& claim : entity["claims"][pid]) {
        auto mainsnak = claim.value("mainsnak", json::object());
        if (mainsnak.value("snaktype", "") != "value") continue;
        auto dv = mainsnak.value("datavalue", json::object());
        if (dv.value("type", "") != "time") continue;
        std::string t = dv.value("value", json::object()).value("time", "");
        if (t.size() < 11) continue;
        std::string iso = t.substr(1, 10); // strip leading '+', take "YYYY-MM-DD"
        int year = 0;
        try { year = std::stoi(iso.substr(0, 4)); } catch (...) { continue; }
        if (year <= 0) continue;
        if (!best || year < best->year) best = WikidataDate{iso, year};
    }
    return best;
}

static std::string labelArray(const std::vector<std::string>& qids,
                               const std::unordered_map<std::string, std::string>& labels) {
    json arr = json::array();
    for (const auto& q : qids) {
        auto it = labels.find(q);
        if (it != labels.end() && !it->second.empty()) arr.push_back(it->second);
    }
    return arr.dump();
}

// ── Wikidata transport ───────────────────────────────────────────────────────

std::vector<WikidataScraper::Candidate>
WikidataScraper::searchAndFilter(const std::string& title, const std::set<std::string>& allowed_p31) {
    std::vector<Candidate> out;

    std::string search_path = "/w/api.php?action=wbsearchentities&search=" + encodeQuery(title)
        + "&language=en&type=item&limit=50&format=json";
    auto search_res = getWithRetry(wikidata_client_, search_path);
    if (!search_res || search_res->status != 200) {
        std::cerr << "[wikidata] search failed: " << (search_res ? search_res->status : 0)
                  << " title=\"" << title << "\"\n";
        return out;
    }

    std::vector<std::string> qids;
    try {
        auto j = json::parse(search_res->body);
        for (const auto& r : j.value("search", json::array())) {
            std::string id = r.value("id", "");
            if (!id.empty()) qids.push_back(id);
        }
    } catch (const std::exception& e) {
        std::cerr << "[wikidata] search parse error: " << e.what() << "\n";
        return out;
    }
    if (qids.empty()) return out;

    // One batched fetch for every candidate's full claims (wbsearchentities
    // itself only returns a label + one-line description, not P31/dates/etc).
    std::string ids_param;
    for (size_t i = 0; i < qids.size(); ++i) { if (i) ids_param += "|"; ids_param += qids[i]; }
    std::string detail_path = "/w/api.php?action=wbgetentities&ids=" + ids_param
        + "&props=labels|descriptions|claims|sitelinks&languages=en&format=json";
    auto detail_res = getWithRetry(wikidata_client_, detail_path);
    if (!detail_res || detail_res->status != 200) {
        std::cerr << "[wikidata] batch entity fetch failed: " << (detail_res ? detail_res->status : 0) << "\n";
        return out;
    }
    try {
        auto j = json::parse(detail_res->body);
        if (!j.contains("entities")) return out;
        for (const auto& qid : qids) {
            if (!j["entities"].contains(qid)) continue;
            const auto& entity = j["entities"][qid];
            if (hasAnyType(entity, allowed_p31)) out.push_back({qid, entity});
        }
    } catch (const std::exception& e) {
        std::cerr << "[wikidata] batch entity parse error: " << e.what() << "\n";
    }
    return out;
}

std::unordered_map<std::string, std::string> WikidataScraper::resolveLabels(const std::set<std::string>& qids) {
    std::unordered_map<std::string, std::string> out;
    if (qids.empty()) return out;
    std::vector<std::string> all(qids.begin(), qids.end());

    // Wikidata caps ids per wbgetentities call at 50.
    for (size_t start = 0; start < all.size(); start += 50) {
        size_t end = std::min(start + 50, all.size());
        std::string ids_param;
        for (size_t i = start; i < end; ++i) { if (i > start) ids_param += "|"; ids_param += all[i]; }
        std::string path = "/w/api.php?action=wbgetentities&ids=" + ids_param
            + "&props=labels&languages=en&format=json";
        auto res = getWithRetry(wikidata_client_, path);
        if (!res || res->status != 200) continue;
        try {
            auto j = json::parse(res->body);
            if (!j.contains("entities")) continue;
            for (auto& [qid, entity] : j["entities"].items())
                out[qid] = labelOf(entity, "en");
        } catch (...) {}
    }
    return out;
}

json WikidataScraper::fetchEntity(const std::string& qid) {
    std::string path = "/w/api.php?action=wbgetentities&ids=" + qid
        + "&props=labels|descriptions|claims|sitelinks&languages=en&format=json";
    auto res = getWithRetry(wikidata_client_, path);
    if (!res || res->status != 200) return json();
    try {
        auto j = json::parse(res->body);
        if (j.contains("entities") && j["entities"].contains(qid)) return j["entities"][qid];
    } catch (const std::exception& e) {
        std::cerr << "[wikidata] fetchEntity parse error: " << e.what() << "\n";
    }
    return json();
}

std::string WikidataScraper::wikipediaOverview(const std::string& enwiki_title, std::string* thumbnail_out) {
    if (enwiki_title.empty()) return "";
    std::string path = "/api/rest_v1/page/summary/" + encodeQuery(enwiki_title);
    auto res = wikipedia_client_.Get(path.c_str());
    if (!res || res->status != 200) return "";
    try {
        auto j = json::parse(res->body);
        if (thumbnail_out && j.contains("thumbnail") && j["thumbnail"].contains("source"))
            *thumbnail_out = j["thumbnail"].value("source", "");
        return j.value("extract", "");
    } catch (...) { return ""; }
}

// ── Entity → Show/Movie ──────────────────────────────────────────────────────

Show WikidataScraper::buildShow(const json& entity, bool full_detail) {
    Show s;
    // No dedicated field for a Wikidata id — stashed in show_id, same
    // convention every other non-TMDB/TVDB scraper here uses.
    s.title    = labelOf(entity, "en");
    s.overview = descriptionOf(entity, "en"); // short blurb; replaced below with a real summary in detail mode

    // Shows prefer start time (P580) — publication date (P577) is more of a
    // film convention, but some show items only carry that one.
    auto date = claimEarliestDate(entity, "P580");
    if (!date) date = claimEarliestDate(entity, "P577");
    if (date) { s.year = date->year; s.originally_available_at = date->iso; }

    // Only set status when there's a clear signal (an end date claim) — no
    // reliable "still airing" property to safely default to otherwise;
    // leaving it blank when unknown is correct, not a bug.
    if (claimEarliestDate(entity, "P582")) s.status = "Ended";

    auto genre_ids   = claimEntityIds(entity, "P136");
    auto country_ids = claimEntityIds(entity, "P495");
    auto network_ids = claimEntityIds(entity, "P449");           // original network
    if (network_ids.empty()) network_ids = claimEntityIds(entity, "P272"); // fall back to production company

    if (full_detail) {
        std::set<std::string> to_resolve(genre_ids.begin(), genre_ids.end());
        to_resolve.insert(country_ids.begin(), country_ids.end());
        if (!network_ids.empty()) to_resolve.insert(network_ids.front());
        auto labels = resolveLabels(to_resolve);

        s.genres    = labelArray(genre_ids, labels);
        s.countries = labelArray(country_ids, labels);
        if (!network_ids.empty()) {
            auto it = labels.find(network_ids.front());
            if (it != labels.end()) s.network = it->second;
        }

        std::string enwiki_title = entity.contains("sitelinks") && entity["sitelinks"].contains("enwiki")
            ? entity["sitelinks"]["enwiki"].value("title", "") : "";
        std::string thumb;
        std::string extract = wikipediaOverview(enwiki_title, &thumb);
        if (!extract.empty()) s.overview = extract;
        if (!thumb.empty())   s.thumb    = thumb;
    }

    return s;
}

Movie WikidataScraper::buildMovie(const json& entity, bool full_detail) {
    Movie m;
    m.title    = labelOf(entity, "en");
    m.overview = descriptionOf(entity, "en");

    auto date = claimEarliestDate(entity, "P577"); // publication date
    if (date) { m.year = date->year; m.release_date = date->iso; }

    auto genre_ids   = claimEntityIds(entity, "P136");
    auto country_ids = claimEntityIds(entity, "P495");
    auto studio_ids  = claimEntityIds(entity, "P272"); // production company
    auto rating_ids  = claimEntityIds(entity, "P1657"); // MPA film rating

    if (full_detail) {
        std::set<std::string> to_resolve(genre_ids.begin(), genre_ids.end());
        to_resolve.insert(country_ids.begin(), country_ids.end());
        if (!studio_ids.empty()) to_resolve.insert(studio_ids.front());
        if (!rating_ids.empty()) to_resolve.insert(rating_ids.front());
        auto labels = resolveLabels(to_resolve);

        m.genres    = labelArray(genre_ids, labels);
        m.countries = labelArray(country_ids, labels);
        if (!studio_ids.empty()) {
            auto it = labels.find(studio_ids.front());
            if (it != labels.end()) m.studio = it->second;
        }
        if (!rating_ids.empty()) {
            auto it = labels.find(rating_ids.front());
            if (it != labels.end()) m.content_rating = it->second;
        }

        std::string enwiki_title = entity.contains("sitelinks") && entity["sitelinks"].contains("enwiki")
            ? entity["sitelinks"]["enwiki"].value("title", "") : "";
        std::string thumb;
        std::string extract = wikipediaOverview(enwiki_title, &thumb);
        if (!extract.empty()) m.overview = extract;
        if (!thumb.empty())   m.thumb    = thumb;
    }

    return m;
}

// ── IMetadataScraper impl ────────────────────────────────────────────────────

std::vector<Show> WikidataScraper::searchShows(const std::string& title, int year) {
    // wbsearchentities has no year filter — ScraperManager's computeScore()
    // handles year scoring against the unfiltered result set instead.
    (void)year;
    auto candidates = searchAndFilter(title, kShowTypes);
    std::vector<Show> out;
    out.reserve(candidates.size());
    for (auto& c : candidates) {
        Show s = buildShow(c.entity, false);
        s.show_id = c.qid;
        out.push_back(std::move(s));
    }
    return out;
}

std::optional<Show> WikidataScraper::fetchShow(const std::string& external_id, const std::string& lang) {
    (void)lang; // English-only — see class comment.
    auto entity = fetchEntity(external_id);
    if (entity.is_null() || entity.empty()) return std::nullopt;
    Show s = buildShow(entity, true);
    s.show_id = external_id;
    return s;
}

std::vector<Episode> WikidataScraper::fetchEpisodes(const std::string&, const std::string&) {
    // Wikidata doesn't reliably carry per-episode items for most shows.
    return {};
}

std::vector<Movie> WikidataScraper::searchMovies(const std::string& title, int year) {
    (void)year;
    auto candidates = searchAndFilter(title, kMovieTypes);
    std::vector<Movie> out;
    out.reserve(candidates.size());
    for (auto& c : candidates) {
        Movie m = buildMovie(c.entity, false);
        m.movie_id = c.qid;
        out.push_back(std::move(m));
    }
    return out;
}

std::optional<Movie> WikidataScraper::fetchMovie(const std::string& external_id, const std::string& lang) {
    (void)lang;
    auto entity = fetchEntity(external_id);
    if (entity.is_null() || entity.empty()) return std::nullopt;
    Movie m = buildMovie(entity, true);
    m.movie_id = external_id;
    return m;
}
