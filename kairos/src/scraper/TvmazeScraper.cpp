#include "TvmazeScraper.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <regex>

using json = nlohmann::json;

static constexpr const char* kBase = "https://api.tvmaze.com";

TvmazeScraper::TvmazeScraper()
    : client_(kBase)
{
    client_.set_connection_timeout(10);
    client_.set_read_timeout(20);
}

httplib::Result TvmazeScraper::get(const std::string& path) {
    return client_.Get(path.c_str());
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

static bool hasInt(const json& j, const std::string& key) {
    return j.contains(key) && j[key].is_number();
}

static int safeInt(const json& j, const std::string& key, int def = 0) {
    return hasInt(j, key) ? j[key].get<int>() : def;
}

static int yearFromDate(const std::string& date) {
    if (date.size() >= 4) {
        try { return std::stoi(date.substr(0, 4)); } catch (...) {}
    }
    return 0;
}

// TVMaze's overview/summary fields are HTML (typically just <p> wrapping) —
// every other scraper in this codebase hands back plain text, so strip tags
// here rather than leaking markup into the UI.
static std::string stripHtml(const std::string& html) {
    static const std::regex kTag("<[^>]*>");
    std::string out = std::regex_replace(html, kTag, "");
    // Collapse the handful of entities TVMaze actually uses; not a full
    // HTML-entity decoder, just enough for real-world summaries.
    static const std::vector<std::pair<std::string, std::string>> kEntities = {
        {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&lt;", "<"}, {"&gt;", ">"},
    };
    for (const auto& [from, to] : kEntities) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return out;
}

static std::string genreArray(const json& genres_j) {
    if (!genres_j.is_array()) return "[]";
    json arr = json::array();
    for (const auto& g : genres_j) {
        if (g.is_string()) arr.push_back(g.get<std::string>());
    }
    return arr.dump();
}

Show TvmazeScraper::showFromJson(const json& j) {
    Show s;
    // TVMaze has no dedicated field on Show for its own id — stashed in
    // show_id, same convention AnidbScraper uses for its AID.
    s.show_id  = std::to_string(safeInt(j, "id"));
    s.title    = safeStr(j, "name");
    s.overview = stripHtml(safeStr(j, "summary"));
    s.status   = safeStr(j, "status");

    std::string premiered = safeStr(j, "premiered");
    int y = yearFromDate(premiered);
    if (y > 0) s.year = y;
    s.originally_available_at = premiered;

    if (j.contains("image") && j["image"].is_object()) {
        s.thumb = safeStr(j["image"], "original");
        if (s.thumb.empty()) s.thumb = safeStr(j["image"], "medium");
    }

    if (j.contains("genres")) s.genres = genreArray(j["genres"]);

    if (j.contains("network") && j["network"].is_object())
        s.network = safeStr(j["network"], "name");
    else if (j.contains("webChannel") && j["webChannel"].is_object())
        s.network = safeStr(j["webChannel"], "name");

    if (j.contains("rating") && j["rating"].is_object() && hasInt(j["rating"], "average"))
        s.audience_rating = j["rating"]["average"].get<float>();

    // Cross-references to TheTVDB/IMDB — let ScraperManager's linkExternalId
    // fill those in as non-primary ids the same way TMDB/TVDB do for each other.
    if (j.contains("externals") && j["externals"].is_object()) {
        const auto& ext = j["externals"];
        if (hasInt(ext, "thetvdb")) s.tvdb_id = std::to_string(ext["thetvdb"].get<int>());
        s.imdb_id = safeStr(ext, "imdb");
    }

    return s;
}

// ── IMetadataScraper impl ────────────────────────────────────────────────────

std::vector<Show> TvmazeScraper::searchShows(const std::string& title, int year) {
    // TVMaze's search has no year parameter — it isn't a hard filter to work
    // around here, computeScore() in ScraperManager applies year scoring
    // against the full result set instead.
    (void)year;
    std::string path = "/search/shows?q=" + encodeQuery(title);

    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[tvmaze] searchShows failed: " << (res ? res->status : 0)
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
        std::cerr << "[tvmaze] searchShows parse error: " << e.what() << "\n";
        return {};
    }
}

std::optional<Show> TvmazeScraper::fetchShow(const std::string& external_id, const std::string& lang) {
    // TVMaze doesn't support per-request language variants.
    (void)lang;
    auto res = get("/shows/" + external_id);
    if (!res || res->status != 200) {
        std::cerr << "[tvmaze] fetchShow " << external_id << " failed: " << (res ? res->status : 0) << "\n";
        return std::nullopt;
    }
    try {
        auto j = json::parse(res->body);
        return showFromJson(j);
    } catch (const std::exception& e) {
        std::cerr << "[tvmaze] fetchShow parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

std::vector<Episode> TvmazeScraper::fetchEpisodes(const std::string& external_id, const std::string& lang) {
    (void)lang;
    // specials=1 includes season-0-style specials alongside regular episodes.
    auto res = get("/shows/" + external_id + "/episodes?specials=1");
    if (!res || res->status != 200) {
        std::cerr << "[tvmaze] fetchEpisodes " << external_id << " failed: " << (res ? res->status : 0) << "\n";
        return {};
    }

    std::vector<Episode> out;
    int special_counter = 0;
    try {
        auto j = json::parse(res->body);
        if (!j.is_array()) return out;
        for (const auto& ep : j) {
            Episode e;
            e.show_id    = external_id;
            e.title      = safeStr(ep, "name");
            e.overview   = stripHtml(safeStr(ep, "summary"));
            e.air_date   = safeStr(ep, "airdate");
            // No dedicated field for a TVMaze episode id — use the generic
            // episode_id slot, prefixed, same convention AnidbScraper uses
            // ("anidb:" + aid) for scanSpecialsForShow's source_episode_id.
            e.episode_id = "tvmaze:" + std::to_string(safeInt(ep, "id"));

            if (ep.contains("image") && ep["image"].is_object()) {
                e.thumb = safeStr(ep["image"], "original");
                if (e.thumb.empty()) e.thumb = safeStr(ep["image"], "medium");
            }

            // Specials commonly report "number": null — number them
            // sequentially under season 0, matching TVDB's convention.
            if (hasInt(ep, "number")) {
                e.season  = safeInt(ep, "season");
                e.episode = safeInt(ep, "number");
            } else {
                e.season  = 0;
                e.episode = ++special_counter;
            }

            out.push_back(std::move(e));
        }
    } catch (const std::exception& e) {
        std::cerr << "[tvmaze] fetchEpisodes parse error: " << e.what() << "\n";
        return {};
    }
    return out;
}

std::vector<Movie> TvmazeScraper::searchMovies(const std::string&, int) { return {}; }
std::optional<Movie> TvmazeScraper::fetchMovie(const std::string&, const std::string&) { return std::nullopt; }
