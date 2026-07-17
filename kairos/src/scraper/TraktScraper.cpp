#include "TraktScraper.h"
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static constexpr const char* kBase = "https://api.trakt.tv";

TraktScraper::TraktScraper(std::string client_id)
    : client_id_(std::move(client_id))
    , client_(kBase)
{
    client_.set_connection_timeout(10);
    client_.set_read_timeout(20);
}

httplib::Result TraktScraper::get(const std::string& path) {
    httplib::Headers hdrs{
        {"Content-Type",     "application/json"},
        {"trakt-api-version", "2"},
        {"trakt-api-key",     client_id_},
    };
    return client_.Get(path.c_str(), hdrs);
}

// ── JSON helpers ─────────────────────────────────────────────────────────────

// See TmdbScraper.cpp/TvdbScraper.cpp — httplib::detail::encode_url() doesn't
// escape a literal '%', which corrupts the query string for titles like
// "200% Wolf". Pre-escape it ourselves before encoding.
static std::string encodeQuery(const std::string& s) {
    std::string escaped;
    escaped.reserve(s.size());
    for (char c : s) {
        if (c == '%') escaped += "%25";
        else escaped += c;
    }
    return httplib::detail::encode_url(escaped);
}

static std::string safeStr(const json& j, const std::string& key, const std::string& def = "") {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

static bool hasNum(const json& j, const std::string& key) {
    return j.contains(key) && j[key].is_number() && !j[key].is_null();
}

static int safeInt(const json& j, const std::string& key, int def = 0) {
    return hasNum(j, key) ? j[key].get<int>() : def;
}

static int yearFromDate(const std::string& date) {
    if (date.size() >= 4) {
        try { return std::stoi(date.substr(0, 4)); } catch (...) {}
    }
    return 0;
}

static std::string genreArray(const json& genres_j) {
    if (!genres_j.is_array()) return "[]";
    json arr = json::array();
    for (const auto& g : genres_j) {
        if (g.is_string()) arr.push_back(g.get<std::string>());
    }
    return arr.dump();
}

// Trakt cross-references live under "ids": {trakt, slug, tvdb, imdb, tmdb}.
struct TraktIds { std::string trakt, tvdb, tmdb, imdb; };
static TraktIds idsFromJson(const json& j) {
    TraktIds ids;
    if (!j.contains("ids") || !j["ids"].is_object()) return ids;
    const auto& idj = j["ids"];
    ids.trakt = std::to_string(safeInt(idj, "trakt"));
    if (hasNum(idj, "tvdb")) ids.tvdb = std::to_string(idj["tvdb"].get<int>());
    if (hasNum(idj, "tmdb")) ids.tmdb = std::to_string(idj["tmdb"].get<int>());
    ids.imdb = safeStr(idj, "imdb");
    return ids;
}

Show TraktScraper::showFromJson(const json& j) {
    Show s;
    auto ids = idsFromJson(j);
    // No dedicated field for a Trakt id — stashed in show_id, same
    // convention AnidbScraper/TvmazeScraper use for their own ids.
    s.show_id  = ids.trakt;
    s.tvdb_id  = ids.tvdb;
    s.tmdb_id  = ids.tmdb;
    s.imdb_id  = ids.imdb;
    s.title    = safeStr(j, "title");
    s.overview = safeStr(j, "overview");
    s.status   = safeStr(j, "status");
    s.content_rating = safeStr(j, "certification");
    s.network  = safeStr(j, "network");

    std::string first_aired = safeStr(j, "first_aired");
    int y = hasNum(j, "year") ? j["year"].get<int>() : yearFromDate(first_aired);
    if (y > 0) s.year = y;
    s.originally_available_at = first_aired;

    if (j.contains("genres")) s.genres = genreArray(j["genres"]);
    if (hasNum(j, "rating")) s.audience_rating = j["rating"].get<float>();

    // Trakt doesn't host poster/backdrop images itself — s.thumb/s.art stay
    // empty. A higher-priority source (or Trakt-derived tmdb_id/tvdb_id via
    // linkExternalId) is expected to supply artwork.
    return s;
}

Movie TraktScraper::movieFromJson(const json& j) {
    Movie m;
    auto ids = idsFromJson(j);
    m.movie_id = ids.trakt;
    m.tmdb_id  = ids.tmdb;
    m.imdb_id  = ids.imdb;
    m.title    = safeStr(j, "title");
    m.overview = safeStr(j, "overview");
    m.tagline  = safeStr(j, "tagline");
    m.content_rating = safeStr(j, "certification");

    std::string released = safeStr(j, "released");
    int y = hasNum(j, "year") ? j["year"].get<int>() : yearFromDate(released);
    if (y > 0) m.year = y;
    m.release_date = released;

    int runtime = safeInt(j, "runtime");
    if (runtime > 0) m.duration_ms = static_cast<int64_t>(runtime) * 60 * 1000;

    if (j.contains("genres")) m.genres = genreArray(j["genres"]);
    if (hasNum(j, "rating")) m.audience_rating = j["rating"].get<float>();

    // No director/writer/studio here — Trakt's standard movie payload
    // doesn't include crew credits (that's a separate /people endpoint,
    // not worth the extra round trip for a secondary metadata source).
    return m;
}

// ── IMetadataScraper impl ────────────────────────────────────────────────────

std::vector<Show> TraktScraper::searchShows(const std::string& title, int year) {
    std::string path = "/search/show?query=" + encodeQuery(title) + "&extended=full";
    if (year > 0) path += "&years=" + std::to_string(year);

    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[trakt] searchShows failed: " << (res ? res->status : 0)
                  << " title=\"" << title << "\""
                  << (res && !res->body.empty() ? " body=" + res->body.substr(0, 120) : "") << "\n";
        return {};
    }
    try {
        auto j = json::parse(res->body);
        std::vector<Show> out;
        if (!j.is_array()) return out;
        for (const auto& item : j) {
            if (!item.contains("show")) continue;
            out.push_back(showFromJson(item["show"]));
        }
        return out;
    } catch (const std::exception& e) {
        std::cerr << "[trakt] searchShows parse error: " << e.what() << "\n";
        return {};
    }
}

std::optional<Show> TraktScraper::fetchShow(const std::string& external_id, const std::string& lang) {
    (void)lang; // Trakt doesn't support per-request language translations.
    auto res = get("/shows/" + external_id + "?extended=full");
    if (!res || res->status != 200) {
        std::cerr << "[trakt] fetchShow " << external_id << " failed: " << (res ? res->status : 0) << "\n";
        return std::nullopt;
    }
    try {
        auto j = json::parse(res->body);
        return showFromJson(j);
    } catch (const std::exception& e) {
        std::cerr << "[trakt] fetchShow parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

std::vector<Episode> TraktScraper::fetchEpisodes(const std::string& external_id, const std::string& lang) {
    (void)lang;
    // One call for every season + episode, unlike TMDB's season-by-season
    // loop — extended=full,episodes embeds each season's episode list.
    auto res = get("/shows/" + external_id + "/seasons?extended=full,episodes");
    if (!res || res->status != 200) {
        std::cerr << "[trakt] fetchEpisodes " << external_id << " failed: " << (res ? res->status : 0) << "\n";
        return {};
    }

    std::vector<Episode> out;
    try {
        auto j = json::parse(res->body);
        if (!j.is_array()) return out;
        for (const auto& season : j) {
            std::string season_name = safeStr(season, "title");
            for (const auto& ep : season.value("episodes", json::array())) {
                Episode e;
                e.show_id  = external_id;
                e.season   = safeInt(ep, "season");
                e.episode  = safeInt(ep, "number");
                e.title    = safeStr(ep, "title");
                e.overview = safeStr(ep, "overview");
                e.air_date = safeStr(ep, "first_aired");
                e.season_name = season_name;
                // No dedicated field for a Trakt episode id — see showFromJson.
                auto ids = idsFromJson(ep);
                if (!ids.trakt.empty()) e.episode_id = "trakt:" + ids.trakt;
                out.push_back(std::move(e));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[trakt] fetchEpisodes parse error: " << e.what() << "\n";
        return {};
    }
    return out;
}

std::vector<Movie> TraktScraper::searchMovies(const std::string& title, int year) {
    std::string path = "/search/movie?query=" + encodeQuery(title) + "&extended=full";
    if (year > 0) path += "&years=" + std::to_string(year);

    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[trakt] searchMovies failed: " << (res ? res->status : 0)
                  << " title=\"" << title << "\""
                  << (res && !res->body.empty() ? " body=" + res->body.substr(0, 120) : "") << "\n";
        return {};
    }
    try {
        auto j = json::parse(res->body);
        std::vector<Movie> out;
        if (!j.is_array()) return out;
        for (const auto& item : j) {
            if (!item.contains("movie")) continue;
            out.push_back(movieFromJson(item["movie"]));
        }
        return out;
    } catch (const std::exception& e) {
        std::cerr << "[trakt] searchMovies parse error: " << e.what() << "\n";
        return {};
    }
}

std::optional<Movie> TraktScraper::fetchMovie(const std::string& external_id, const std::string& lang) {
    (void)lang;
    auto res = get("/movies/" + external_id + "?extended=full");
    if (!res || res->status != 200) {
        std::cerr << "[trakt] fetchMovie " << external_id << " failed: " << (res ? res->status : 0) << "\n";
        return std::nullopt;
    }
    try {
        auto j = json::parse(res->body);
        return movieFromJson(j);
    } catch (const std::exception& e) {
        std::cerr << "[trakt] fetchMovie parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}
