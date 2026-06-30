#include "TmdbScraper.h"
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static constexpr const char* kBase     = "https://api.themoviedb.org";
static constexpr const char* kImgBase  = "https://image.tmdb.org/t/p/w500";

TmdbScraper::TmdbScraper(std::string api_key, std::string language)
    : api_key_(std::move(api_key))
    , language_(std::move(language))
    , client_(kBase)
{
    client_.set_connection_timeout(10);
    client_.set_read_timeout(20);
}

std::string TmdbScraper::posterUrl(const std::string& path) {
    if (path.empty()) return "";
    return std::string(kImgBase) + (path[0] == '/' ? path : "/" + path);
}

httplib::Result TmdbScraper::get(const std::string& path) {
    return client_.Get(path.c_str());
}

// ── JSON helpers ─────────────────────────────────────────────────────────────

static std::string safeStr(const json& j, const std::string& key, const std::string& def = "") {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

static int safeInt(const json& j, const std::string& key, int def = 0) {
    if (j.contains(key)) {
        if (j[key].is_number()) return j[key].get<int>();
        if (j[key].is_string()) {
            try { return std::stoi(j[key].get<std::string>()); } catch (...) {}
        }
    }
    return def;
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
        if (g.contains("name")) arr.push_back(g["name"].get<std::string>());
    }
    return arr.dump();
}

Show TmdbScraper::showFromJson(const json& j, bool is_detail) {
    Show s;
    s.tmdb_id = std::to_string(safeInt(j, "id"));
    s.title   = safeStr(j, "name");
    s.overview = safeStr(j, "overview");

    std::string first_air = safeStr(j, "first_air_date");
    int y = yearFromDate(first_air);
    if (y > 0) s.year = y;
    s.originally_available_at = first_air;

    s.thumb = posterUrl(safeStr(j, "poster_path"));
    s.art   = posterUrl(safeStr(j, "backdrop_path"));

    if (j.contains("vote_average") && j["vote_average"].is_number())
        s.audience_rating = j["vote_average"].get<float>();

    if (is_detail) {
        s.status = safeStr(j, "status");
        s.genres = genreArray(j.value("genres", json::array()));

        if (j.contains("networks") && j["networks"].is_array() && !j["networks"].empty())
            s.network = safeStr(j["networks"][0], "name");

        if (j.contains("production_companies") && j["production_companies"].is_array()
                && !j["production_companies"].empty())
            s.studio = safeStr(j["production_companies"][0], "name");

        // external IDs from append_to_response=external_ids
        if (j.contains("external_ids")) {
            const auto& ext = j["external_ids"];
            s.imdb_id = safeStr(ext, "imdb_id");
            int tvdb = safeInt(ext, "tvdb_id");
            if (tvdb > 0) s.tvdb_id = std::to_string(tvdb);
        }

        // content ratings
        if (j.contains("content_ratings") && j["content_ratings"].contains("results")) {
            for (const auto& r : j["content_ratings"]["results"]) {
                if (safeStr(r, "iso_3166_1") == "US") {
                    s.content_rating = safeStr(r, "rating");
                    break;
                }
            }
        }
    }

    return s;
}

Movie TmdbScraper::movieFromJson(const json& j, bool is_detail) {
    Movie m;
    m.tmdb_id  = std::to_string(safeInt(j, "id"));
    m.title    = safeStr(j, "title");
    m.overview = safeStr(j, "overview");
    m.tagline  = safeStr(j, "tagline");

    std::string release = safeStr(j, "release_date");
    int y = yearFromDate(release);
    if (y > 0) m.year = y;

    m.thumb = posterUrl(safeStr(j, "poster_path"));
    m.art   = posterUrl(safeStr(j, "backdrop_path"));

    if (j.contains("vote_average") && j["vote_average"].is_number())
        m.audience_rating = j["vote_average"].get<float>();

    if (is_detail) {
        int runtime = safeInt(j, "runtime");
        m.duration_ms = static_cast<int64_t>(runtime) * 60 * 1000;
        m.genres = genreArray(j.value("genres", json::array()));

        if (j.contains("production_companies") && j["production_companies"].is_array()
                && !j["production_companies"].empty())
            m.studio = safeStr(j["production_companies"][0], "name");

        if (j.contains("release_dates") && j["release_dates"].contains("results")) {
            for (const auto& r : j["release_dates"]["results"]) {
                if (safeStr(r, "iso_3166_1") == "US") {
                    if (r.contains("release_dates") && r["release_dates"].is_array()
                            && !r["release_dates"].empty()) {
                        m.content_rating = safeStr(r["release_dates"][0], "certification");
                    }
                    break;
                }
            }
        }

        if (j.contains("external_ids"))
            m.imdb_id = safeStr(j["external_ids"], "imdb_id");

        // director from credits
        if (j.contains("credits") && j["credits"].contains("crew")) {
            for (const auto& c : j["credits"]["crew"]) {
                if (safeStr(c, "job") == "Director") {
                    m.director = safeStr(c, "name");
                    break;
                }
            }
        }
    }

    return m;
}

Episode TmdbScraper::episodeFromJson(const json& j, const std::string& show_id) {
    Episode e;
    e.show_id     = show_id;
    e.season      = safeInt(j, "season_number");
    e.episode     = safeInt(j, "episode_number");
    e.title       = safeStr(j, "name");
    e.overview    = safeStr(j, "overview");
    e.air_date    = safeStr(j, "air_date");
    e.thumb       = posterUrl(safeStr(j, "still_path"));
    e.tmdb_id     = std::to_string(safeInt(j, "id"));

    if (j.contains("vote_average") && j["vote_average"].is_number()) {
        // store in audience_rating field (not in Episode struct — skip)
    }
    return e;
}

// ── IMetadataScraper impl ────────────────────────────────────────────────────

std::vector<Show> TmdbScraper::searchShows(const std::string& title, int year) {
    std::string path = "/3/search/tv?api_key=" + api_key_
        + "&language=" + language_
        + "&query=" + httplib::detail::encode_url(title);
    if (year > 0) path += "&first_air_date_year=" + std::to_string(year);

    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[tmdb] searchShows failed: " << (res ? res->status : 0)
                  << " title=\"" << title << "\""
                  << (res && !res->body.empty() ? " body=" + res->body.substr(0, 120) : "") << "\n";
        return {};
    }

    try {
        auto j = json::parse(res->body);
        std::vector<Show> out;
        for (const auto& item : j.value("results", json::array())) {
            out.push_back(showFromJson(item, false));
        }
        return out;
    } catch (const std::exception& e) {
        std::cerr << "[tmdb] searchShows parse error: " << e.what() << "\n";
        return {};
    }
}

std::optional<Show> TmdbScraper::fetchShow(const std::string& external_id, const std::string& lang) {
    std::string path = "/3/tv/" + external_id
        + "?api_key=" + api_key_
        + "&language=" + (lang.empty() ? language_ : lang)
        + "&append_to_response=external_ids,content_ratings";

    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[tmdb] fetchShow " << external_id << " failed: " << (res ? res->status : 0) << "\n";
        return std::nullopt;
    }

    try {
        auto j = json::parse(res->body);
        return showFromJson(j, true);
    } catch (const std::exception& e) {
        std::cerr << "[tmdb] fetchShow parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

std::vector<Episode> TmdbScraper::fetchEpisodes(const std::string& external_id, const std::string& lang) {
    const std::string& eff_lang = lang.empty() ? language_ : lang;
    // First fetch show to get season list
    std::string show_path = "/3/tv/" + external_id
        + "?api_key=" + api_key_ + "&language=" + eff_lang;
    auto show_res = get(show_path);
    if (!show_res || show_res->status != 200) return {};

    std::vector<int> season_nums;
    try {
        auto sj = json::parse(show_res->body);
        for (const auto& s : sj.value("seasons", json::array())) {
            int n = safeInt(s, "season_number");
            if (n > 0) season_nums.push_back(n);  // skip specials (season 0)
        }
    } catch (...) { return {}; }

    std::vector<Episode> out;
    for (int season : season_nums) {
        std::string path = "/3/tv/" + external_id + "/season/" + std::to_string(season)
            + "?api_key=" + api_key_ + "&language=" + eff_lang;
        auto res = get(path);
        if (!res || res->status != 200) continue;

        try {
            auto j = json::parse(res->body);
            std::string season_name = safeStr(j, "name");
            for (const auto& ep : j.value("episodes", json::array())) {
                auto e = episodeFromJson(ep, external_id);
                e.season_name = season_name;
                out.push_back(std::move(e));
            }
        } catch (...) { continue; }
    }
    return out;
}

std::vector<Movie> TmdbScraper::searchMovies(const std::string& title, int year) {
    std::string path = "/3/search/movie?api_key=" + api_key_
        + "&language=" + language_
        + "&query=" + httplib::detail::encode_url(title);
    if (year > 0) path += "&year=" + std::to_string(year);

    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[tmdb] searchMovies failed: " << (res ? res->status : 0)
                  << " title=\"" << title << "\""
                  << (res && !res->body.empty() ? " body=" + res->body.substr(0, 120) : "") << "\n";
        return {};
    }

    try {
        auto j = json::parse(res->body);
        std::vector<Movie> out;
        for (const auto& item : j.value("results", json::array())) {
            out.push_back(movieFromJson(item, false));
        }
        return out;
    } catch (const std::exception& e) {
        std::cerr << "[tmdb] searchMovies parse error: " << e.what() << "\n";
        return {};
    }
}

std::optional<Movie> TmdbScraper::fetchMovie(const std::string& external_id, const std::string& lang) {
    std::string path = "/3/movie/" + external_id
        + "?api_key=" + api_key_
        + "&language=" + (lang.empty() ? language_ : lang)
        + "&append_to_response=external_ids,release_dates,credits";

    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[tmdb] fetchMovie " << external_id << " failed: " << (res ? res->status : 0) << "\n";
        return std::nullopt;
    }

    try {
        auto j = json::parse(res->body);
        return movieFromJson(j, true);
    } catch (const std::exception& e) {
        std::cerr << "[tmdb] fetchMovie parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}
