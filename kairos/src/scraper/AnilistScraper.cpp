#include "AnilistScraper.h"
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static constexpr const char* kBase = "https://graphql.anilist.co";

AnilistScraper::AnilistScraper()
    : client_(kBase)
{
    client_.set_connection_timeout(10);
    client_.set_read_timeout(20);
}

// ── GraphQL transport ────────────────────────────────────────────────────────

std::optional<json> AnilistScraper::post(const std::string& query, const json& variables) {
    json body;
    body["query"]     = query;
    body["variables"] = variables;

    auto res = client_.Post("/", body.dump(), "application/json");
    if (!res) {
        std::cerr << "[anilist] request failed (no response)\n";
        return std::nullopt;
    }
    // AniList returns 200 for partial-success responses and 400/404 for hard
    // query errors — either way, check the parsed body for "errors" rather
    // than trusting the HTTP status alone.
    try {
        auto j = json::parse(res->body);
        if (j.contains("errors")) {
            std::string msg = j["errors"].is_array() && !j["errors"].empty()
                ? j["errors"][0].value("message", "unknown error") : "unknown error";
            std::cerr << "[anilist] GraphQL error: " << msg << "\n";
            return std::nullopt;
        }
        if (res->status != 200 || !j.contains("data") || j["data"].is_null()) return std::nullopt;
        return j["data"];
    } catch (const std::exception& e) {
        std::cerr << "[anilist] response parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

// ── JSON helpers ─────────────────────────────────────────────────────────────

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

static std::string genreArray(const json& genres_j) {
    if (!genres_j.is_array()) return "[]";
    json arr = json::array();
    for (const auto& g : genres_j) {
        if (g.is_string()) arr.push_back(g.get<std::string>());
    }
    return arr.dump();
}

static std::string titleFromJson(const json& j) {
    if (!j.contains("title") || !j["title"].is_object()) return "";
    std::string en = safeStr(j["title"], "english");
    return en.empty() ? safeStr(j["title"], "romaji") : en;
}

static std::string fuzzyDate(const json& j, const std::string& key) {
    if (!j.contains(key) || !j[key].is_object()) return "";
    const auto& d = j[key];
    int y = safeInt(d, "year"), m = safeInt(d, "month"), day = safeInt(d, "day");
    if (y <= 0) return "";
    char buf[16];
    if (m > 0 && day > 0) std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, day);
    else                  std::snprintf(buf, sizeof(buf), "%04d-01-01", y);
    return buf;
}

static std::string studioFromJson(const json& j) {
    if (!j.contains("studios") || !j["studios"].contains("nodes")) return "";
    const auto& nodes = j["studios"]["nodes"];
    if (!nodes.is_array() || nodes.empty()) return "";
    return safeStr(nodes[0], "name");
}

static std::string coverFromJson(const json& j) {
    if (!j.contains("coverImage") || !j["coverImage"].is_object()) return "";
    std::string url = safeStr(j["coverImage"], "extraLarge");
    return url.empty() ? safeStr(j["coverImage"], "large") : url;
}

Show AnilistScraper::showFromJson(const json& j) {
    Show s;
    // No dedicated field for an AniList id — stashed in show_id, same
    // convention AnidbScraper/TvmazeScraper/TraktScraper use for theirs.
    s.show_id  = std::to_string(safeInt(j, "id"));
    s.title    = titleFromJson(j);
    s.overview = safeStr(j, "description");
    s.status   = safeStr(j, "status");
    s.thumb    = coverFromJson(j);
    s.art      = safeStr(j, "bannerImage");
    s.studio   = studioFromJson(j);
    s.network  = s.studio;

    std::string start = fuzzyDate(j, "startDate");
    if (!start.empty()) {
        try { s.year = std::stoi(start.substr(0, 4)); } catch (...) {}
    }
    s.originally_available_at = start;

    if (j.contains("genres")) s.genres = genreArray(j["genres"]);
    // AniList scores 0-100 — normalize to the 0-10 scale every other
    // scraper in this codebase uses for audience_rating.
    if (hasNum(j, "averageScore")) s.audience_rating = j["averageScore"].get<float>() / 10.0f;

    return s;
}

Movie AnilistScraper::movieFromJson(const json& j) {
    Movie m;
    m.movie_id = std::to_string(safeInt(j, "id"));
    m.title    = titleFromJson(j);
    m.overview = safeStr(j, "description");
    m.studio   = studioFromJson(j);
    m.thumb    = coverFromJson(j);
    m.art      = safeStr(j, "bannerImage");

    std::string start = fuzzyDate(j, "startDate");
    if (!start.empty()) {
        try { m.year = std::stoi(start.substr(0, 4)); } catch (...) {}
    }
    m.release_date = start;

    int duration = safeInt(j, "duration");
    if (duration > 0) m.duration_ms = static_cast<int64_t>(duration) * 60 * 1000;

    if (j.contains("genres")) m.genres = genreArray(j["genres"]);
    if (hasNum(j, "averageScore")) m.audience_rating = j["averageScore"].get<float>() / 10.0f;

    return m;
}

// ── GraphQL query text ───────────────────────────────────────────────────────

static constexpr const char* kMediaFields =
    "id title { romaji english } startDate { year month day } status genres "
    "coverImage { extraLarge large } bannerImage description(asHtml: false) "
    "averageScore studios(isMain: true) { nodes { name } } duration";

// ── IMetadataScraper impl ────────────────────────────────────────────────────

std::vector<Show> AnilistScraper::searchShows(const std::string& title, int year) {
    static const std::string kQuery = std::string(
        "query ($search: String, $seasonYear: Int) { Page(page: 1, perPage: 10) { "
        "media(search: $search, type: ANIME, format_not: MOVIE, seasonYear: $seasonYear, "
        "sort: SEARCH_MATCH) { ") + kMediaFields + " } } }";

    json vars;
    vars["search"]     = title;
    vars["seasonYear"] = year > 0 ? json(year) : json(nullptr);

    auto data = post(kQuery, vars);
    std::vector<Show> out;
    if (!data) return out;
    try {
        for (const auto& item : (*data)["Page"]["media"])
            out.push_back(showFromJson(item));
    } catch (const std::exception& e) {
        std::cerr << "[anilist] searchShows parse error: " << e.what() << "\n";
    }
    return out;
}

std::optional<Show> AnilistScraper::fetchShow(const std::string& external_id, const std::string& lang) {
    (void)lang; // AniList doesn't support per-request language translations.
    static const std::string kQuery = std::string(
        "query ($id: Int) { Media(id: $id, type: ANIME) { ") + kMediaFields + " } }";

    json vars; try { vars["id"] = std::stoi(external_id); } catch (...) { return std::nullopt; }
    auto data = post(kQuery, vars);
    if (!data) return std::nullopt;
    try {
        return showFromJson((*data)["Media"]);
    } catch (const std::exception& e) {
        std::cerr << "[anilist] fetchShow parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

std::vector<Episode> AnilistScraper::fetchEpisodes(const std::string&, const std::string&) {
    // AniList's public schema has no per-episode titles/overviews/air
    // dates — only a total episode count on the Media object itself.
    // Nothing useful to return here.
    return {};
}

std::vector<Movie> AnilistScraper::searchMovies(const std::string& title, int year) {
    static const std::string kQuery = std::string(
        "query ($search: String, $seasonYear: Int) { Page(page: 1, perPage: 10) { "
        "media(search: $search, type: ANIME, format: MOVIE, seasonYear: $seasonYear, "
        "sort: SEARCH_MATCH) { ") + kMediaFields + " } } }";

    json vars;
    vars["search"]     = title;
    vars["seasonYear"] = year > 0 ? json(year) : json(nullptr);

    auto data = post(kQuery, vars);
    std::vector<Movie> out;
    if (!data) return out;
    try {
        for (const auto& item : (*data)["Page"]["media"])
            out.push_back(movieFromJson(item));
    } catch (const std::exception& e) {
        std::cerr << "[anilist] searchMovies parse error: " << e.what() << "\n";
    }
    return out;
}

std::optional<Movie> AnilistScraper::fetchMovie(const std::string& external_id, const std::string& lang) {
    (void)lang;
    static const std::string kQuery = std::string(
        "query ($id: Int) { Media(id: $id, type: ANIME) { ") + kMediaFields + " } }";

    json vars; try { vars["id"] = std::stoi(external_id); } catch (...) { return std::nullopt; }
    auto data = post(kQuery, vars);
    if (!data) return std::nullopt;
    try {
        return movieFromJson((*data)["Media"]);
    } catch (const std::exception& e) {
        std::cerr << "[anilist] fetchMovie parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}
