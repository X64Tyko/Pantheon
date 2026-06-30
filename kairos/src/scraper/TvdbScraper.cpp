#include "TvdbScraper.h"
#include <ctime>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static constexpr const char* kBase = "https://api4.thetvdb.com";
// TVDB tokens are valid for 30 days; refresh 1 day before expiry.
static constexpr int64_t kTokenTtl = (30 - 1) * 24 * 3600;

TvdbScraper::TvdbScraper(std::string api_key, std::string language, std::string pin)
    : api_key_(std::move(api_key))
    , language_(std::move(language))
    , pin_(std::move(pin))
    , client_(kBase)
{
    client_.set_connection_timeout(10);
    client_.set_read_timeout(20);
}

bool TvdbScraper::ensureToken() {
    std::lock_guard lock(token_mu_);
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (!token_.empty() && now < token_expiry_) return true;

    json body;
    body["apikey"] = api_key_;
    if (!pin_.empty()) body["pin"] = pin_;

    auto res = client_.Post("/v4/login", body.dump(), "application/json");
    if (!res || res->status != 200) {
        std::cerr << "[tvdb] login failed: " << (res ? res->status : 0) << "\n";
        return false;
    }
    try {
        auto j = json::parse(res->body);
        if (j.value("status", "") != "success") return false;
        token_        = j["data"]["token"].get<std::string>();
        token_expiry_ = now + kTokenTtl;
        return true;
    } catch (...) {
        std::cerr << "[tvdb] login parse error\n";
        return false;
    }
}

httplib::Result TvdbScraper::get(const std::string& path) {
    if (!ensureToken()) return httplib::Result{};
    httplib::Headers hdrs{{"Authorization", "Bearer " + token_}};
    return client_.Get(path.c_str(), hdrs);
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

static int yearFromDate(const std::string& d) {
    if (d.size() >= 4) { try { return std::stoi(d.substr(0, 4)); } catch (...) {} }
    return 0;
}

static std::string genreArray(const json& genres) {
    if (!genres.is_array()) return "[]";
    json arr = json::array();
    for (const auto& g : genres) {
        std::string name = safeStr(g, "name");
        if (!name.empty()) arr.push_back(name);
    }
    return arr.dump();
}

// ISO 639-1 (2-letter) → ISO 639-2B (3-letter) for the TVDB translations API.
static std::string toLang3(const std::string& lang) {
    if (lang == "en") return "eng";
    if (lang == "ja") return "jpn";
    if (lang == "ko") return "kor";
    if (lang == "zh") return "zho";
    if (lang == "fr") return "fra";
    if (lang == "de") return "deu";
    if (lang == "es") return "spa";
    if (lang == "it") return "ita";
    if (lang == "pt") return "por";
    if (lang == "ru") return "rus";
    if (lang == "ar") return "ara";
    if (lang == "nl") return "nld";
    if (lang == "pl") return "pol";
    if (lang == "sv") return "swe";
    if (lang == "tr") return "tur";
    if (lang.size() == 3) return lang;  // already 3-letter, pass through
    return "eng";
}

Show TvdbScraper::showFromJson(const json& j) {
    Show s;
    int id = safeInt(j, "id");
    s.tvdb_id = id > 0 ? std::to_string(id) : safeStr(j, "tvdb_id");
    s.title   = safeStr(j, "name");
    s.overview = safeStr(j, "overview");
    s.status  = safeStr(j, "status");
    if (j.contains("status") && j["status"].is_object())
        s.status = safeStr(j["status"], "name");

    std::string first_air = safeStr(j, "firstAired");
    if (first_air.empty()) first_air = safeStr(j, "first_air_time");
    int y = yearFromDate(first_air);
    if (y > 0) s.year = y;
    s.originally_available_at = first_air;

    s.thumb = safeStr(j, "image");
    if (s.thumb.empty()) s.thumb = safeStr(j, "image_url");
    if (s.thumb.empty()) s.thumb = safeStr(j, "imageUrl");

    if (j.contains("genres"))     s.genres  = genreArray(j["genres"]);
    if (j.contains("genreIds"))   {} // genre IDs only — skip

    if (j.contains("network") && j["network"].is_object())
        s.network = safeStr(j["network"], "name");
    else if (j.contains("network"))
        s.network = safeStr(j, "network");

    // remote IDs (TMDB, IMDB)
    if (j.contains("remoteIds") && j["remoteIds"].is_array()) {
        for (const auto& r : j["remoteIds"]) {
            std::string src = safeStr(r, "sourceName");
            std::string val = safeStr(r, "id");
            if (src == "TheMovieDB.com" || src == "TMDB") s.tmdb_id = val;
            else if (src == "IMDB") s.imdb_id = val;
        }
    }

    return s;
}

Movie TvdbScraper::movieFromJson(const json& j) {
    Movie m;
    int id = safeInt(j, "id");
    m.tmdb_id = "";  // TVDB movie IDs aren't TMDB
    // Store TVDB ID in imdb_id temporarily — no dedicated field
    // Actually Movie struct has tmdb_id only. Use that for TVDB movie ID prefixed.
    // Better: Store as "" and rely on remoteIds for TMDB/IMDB
    m.title    = safeStr(j, "name");
    m.overview = safeStr(j, "overview");

    std::string release = safeStr(j, "first_release");
    if (release.empty() && j.contains("releases") && j["releases"].is_array()
            && !j["releases"].empty()) {
        release = safeStr(j["releases"][0], "date");
    }
    int y = yearFromDate(release);
    if (y > 0) m.year = y;

    m.thumb = safeStr(j, "image");
    if (m.thumb.empty()) m.thumb = safeStr(j, "image_url");

    if (j.contains("genres"))   m.genres = genreArray(j["genres"]);

    // studios
    if (j.contains("studios") && j["studios"].is_array() && !j["studios"].empty())
        m.studio = safeStr(j["studios"][0], "name");

    // remote IDs
    if (j.contains("remoteIds") && j["remoteIds"].is_array()) {
        for (const auto& r : j["remoteIds"]) {
            std::string src = safeStr(r, "sourceName");
            std::string val = safeStr(r, "id");
            if (src == "TheMovieDB.com" || src == "TMDB") m.tmdb_id = val;
            else if (src == "IMDB") m.imdb_id = val;
        }
    }

    (void)id;
    return m;
}

Episode TvdbScraper::episodeFromJson(const json& j, const std::string& show_id) {
    Episode e;
    e.show_id  = show_id;
    e.season   = safeInt(j, "seasonNumber");
    e.episode  = safeInt(j, "number");
    e.title    = safeStr(j, "name");
    e.overview = safeStr(j, "overview");
    e.air_date = safeStr(j, "aired");
    e.thumb    = safeStr(j, "image");
    e.tvdb_id  = std::to_string(safeInt(j, "id"));
    int abs = safeInt(j, "absoluteNumber", -1);
    if (abs >= 0) e.absolute_index = abs;
    return e;
}

// ── IMetadataScraper impl ────────────────────────────────────────────────────

std::vector<Show> TvdbScraper::searchShows(const std::string& title, int year) {
    std::string path = "/v4/search?type=series&query="
        + httplib::detail::encode_url(title);
    if (year > 0) path += "&year=" + std::to_string(year);

    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[tvdb] searchShows failed: " << (res ? res->status : 0)
                  << " title=\"" << title << "\""
                  << (res && !res->body.empty() ? " body=" + res->body.substr(0, 120) : "") << "\n";
        return {};
    }
    try {
        auto j = json::parse(res->body);
        if (j.value("status", "") != "success") return {};
        std::vector<Show> out;
        for (const auto& item : j.value("data", json::array())) {
            out.push_back(showFromJson(item));
        }
        return out;
    } catch (const std::exception& e) {
        std::cerr << "[tvdb] searchShows parse: " << e.what() << "\n";
        return {};
    }
}

std::optional<Show> TvdbScraper::fetchShow(const std::string& external_id, const std::string& lang) {
    std::string path = "/v4/series/" + external_id
        + "/extended?meta=episodes&short=true";

    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[tvdb] fetchShow " << external_id << " failed: " << (res ? res->status : 0) << "\n";
        return std::nullopt;
    }
    std::optional<Show> show;
    try {
        auto j = json::parse(res->body);
        if (j.value("status", "") != "success") return std::nullopt;
        show = showFromJson(j["data"]);
    } catch (const std::exception& e) {
        std::cerr << "[tvdb] fetchShow parse: " << e.what() << "\n";
        return std::nullopt;
    }

    // Override name and overview with the requested language's translation.
    const std::string lang3 = toLang3(lang.empty() ? language_ : lang);
    if (lang3 != "eng" && show) {
        auto tr = get("/v4/series/" + external_id + "/translations/" + lang3);
        if (tr && tr->status == 200) {
            try {
                auto tj = json::parse(tr->body);
                if (tj.value("status", "") == "success" && tj.contains("data")) {
                    const auto& d = tj["data"];
                    std::string name = safeStr(d, "name");
                    std::string overview = safeStr(d, "overview");
                    if (!name.empty())     show->title    = name;
                    if (!overview.empty()) show->overview = overview;
                }
            } catch (...) {}
        }
    }
    return show;
}

std::vector<Episode> TvdbScraper::fetchEpisodes(const std::string& external_id, const std::string& lang) {
    const std::string lang3 = toLang3(lang.empty() ? language_ : lang);
    std::vector<Episode> out;
    int page = 0;
    while (true) {
        std::string path = "/v4/series/" + external_id
            + "/episodes/official?page=" + std::to_string(page)
            + "&lang=" + lang3;
        auto res = get(path);
        if (!res || res->status != 200) break;

        try {
            auto j = json::parse(res->body);
            if (j.value("status", "") != "success") break;
            auto& data = j["data"];
            json empty_arr = json::array();
            auto& eps  = data.contains("episodes") ? data["episodes"] : empty_arr;
            if (eps.empty()) break;
            for (const auto& ep : eps)
                out.push_back(episodeFromJson(ep, external_id));
            // TVDB paginates at 100/page; if < 100, we're done
            if ((int)eps.size() < 100) break;
            ++page;
        } catch (...) { break; }
    }
    return out;
}

std::vector<Movie> TvdbScraper::searchMovies(const std::string& title, int year) {
    std::string path = "/v4/search?type=movie&query="
        + httplib::detail::encode_url(title);
    if (year > 0) path += "&year=" + std::to_string(year);

    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[tvdb] searchMovies failed: " << (res ? res->status : 0)
                  << " title=\"" << title << "\""
                  << (res && !res->body.empty() ? " body=" + res->body.substr(0, 120) : "") << "\n";
        return {};
    }
    try {
        auto j = json::parse(res->body);
        if (j.value("status", "") != "success") return {};
        std::vector<Movie> out;
        for (const auto& item : j.value("data", json::array())) {
            out.push_back(movieFromJson(item));
        }
        return out;
    } catch (const std::exception& e) {
        std::cerr << "[tvdb] searchMovies parse: " << e.what() << "\n";
        return {};
    }
}

std::optional<Movie> TvdbScraper::fetchMovie(const std::string& external_id, const std::string& lang) {
    std::string path = "/v4/movies/" + external_id + "/extended";
    auto res = get(path);
    if (!res || res->status != 200) {
        std::cerr << "[tvdb] fetchMovie " << external_id << " failed: " << (res ? res->status : 0) << "\n";
        return std::nullopt;
    }
    std::optional<Movie> movie;
    try {
        auto j = json::parse(res->body);
        if (j.value("status", "") != "success") return std::nullopt;
        movie = movieFromJson(j["data"]);
    } catch (const std::exception& e) {
        std::cerr << "[tvdb] fetchMovie parse: " << e.what() << "\n";
        return std::nullopt;
    }

    const std::string lang3 = toLang3(lang.empty() ? language_ : lang);
    if (lang3 != "eng" && movie) {
        auto tr = get("/v4/movies/" + external_id + "/translations/" + lang3);
        if (tr && tr->status == 200) {
            try {
                auto tj = json::parse(tr->body);
                if (tj.value("status", "") == "success" && tj.contains("data")) {
                    const auto& d = tj["data"];
                    std::string name     = safeStr(d, "name");
                    std::string overview = safeStr(d, "overview");
                    if (!name.empty())     movie->title    = name;
                    if (!overview.empty()) movie->overview = overview;
                }
            } catch (...) {}
        }
    }
    return movie;
}
