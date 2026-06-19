#include "PlexSource.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

// ---------------------------------------------------------------------------

PlexSource::PlexSource(const std::string& source_id,
                       const std::string& base_url,
                       const std::string& token)
    : source_id_(source_id), base_url_(base_url), token_(token)
    , client_(base_url)
{
    client_.set_default_headers({
        {"X-Plex-Token", token_},
        {"Accept",       "application/json"}
    });
    client_.set_connection_timeout(10);
    client_.set_read_timeout(30);
}

httplib::Result PlexSource::get(const std::string& path) {
    auto res = client_.Get(path);
    if (!res)
        std::cerr << "[plex:" << source_id_ << "] " << path
                  << " — " << httplib::to_string(res.error()) << '\n';
    else if (res->status != 200)
        std::cerr << "[plex:" << source_id_ << "] " << path
                  << " — HTTP " << res->status << '\n';
    return res;
}

// ---------------------------------------------------------------------------

std::vector<LibraryInfo> PlexSource::listAvailableLibraries() {
    auto res = get("/library/sections");
    if (!res || res->status != 200) return {};

    std::vector<LibraryInfo> result;
    try {
        auto j = json::parse(res->body);
        for (const auto& dir : j["MediaContainer"]["Directory"]) {
            std::string plex_type = dir.value("type", "");
            if (plex_type != "show" && plex_type != "movie") continue;

            LibraryInfo info;
            info.external_lib_id = dir["key"].get<std::string>();
            info.name            = dir["title"].get<std::string>();
            info.type            = (plex_type == "show") ? "show" : "movie";
            result.push_back(std::move(info));
        }
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] parse error (sections): " << e.what() << '\n';
    }
    return result;
}

// ---------------------------------------------------------------------------

std::vector<Show> PlexSource::fetchShows(const std::string& external_lib_id) {
    auto res = get("/library/sections/" + external_lib_id + "/all?type=2");
    if (!res || res->status != 200) return {};

    std::vector<Show> result;
    try {
        auto j = json::parse(res->body);
        const auto& items = j["MediaContainer"]["Metadata"];
        result.reserve(items.size());
        for (const auto& item : items) {
            Show show;
            show.show_id        = item["ratingKey"].get<std::string>();
            show.title          = item["title"].get<std::string>();
            show.content_rating = item.value("contentRating", "");
            show.overview       = item.value("summary", "");
            show.studio         = item.value("studio", "");
            show.status         = item.value("status", "");
            show.thumb          = item.value("thumb", "");
            show.art            = item.value("art", "");
            show.originally_available_at = item.value("originallyAvailableAt", "");
            if (item.contains("year") && !item["year"].is_null())
                show.year = item["year"].get<int>();
            if (item.contains("audienceRating") && !item["audienceRating"].is_null())
                show.audience_rating = item["audienceRating"].get<float>();

            // Genres: [{"tag":"Drama"}, ...]
            json genres = json::array();
            if (item.contains("Genre"))
                for (const auto& g : item["Genre"])
                    genres.push_back(g.value("tag", ""));
            show.genres = genres.dump();

            // External IDs: [{"id":"imdb://tt..."}, {"id":"tvdb://..."}, ...]
            if (item.contains("Guid")) {
                for (const auto& g : item["Guid"]) {
                    std::string id = g.value("id", "");
                    if (id.rfind("imdb://", 0) == 0)  show.imdb_id = id.substr(7);
                    if (id.rfind("tvdb://", 0) == 0)  show.tvdb_id = id.substr(7);
                    if (id.rfind("tmdb://", 0) == 0)  show.tmdb_id = id.substr(7);
                }
            }
            result.push_back(std::move(show));
        }
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] parse error (shows): " << e.what() << '\n';
    }
    return result;
}

// ---------------------------------------------------------------------------

std::vector<Movie> PlexSource::fetchMovies(const std::string& external_lib_id) {
    auto res = get("/library/sections/" + external_lib_id + "/all?type=1");
    if (!res || res->status != 200) return {};

    std::vector<Movie> result;
    try {
        auto j = json::parse(res->body);
        for (const auto& item : j["MediaContainer"]["Metadata"]) {
            std::string file_path;
            if (item.contains("Media") && !item["Media"].empty()) {
                const auto& media = item["Media"][0];
                if (media.contains("Part") && !media["Part"].empty())
                    file_path = media["Part"][0].value("file", "");
            }
            if (file_path.empty()) continue;

            Movie movie;
            movie.movie_id       = item["ratingKey"].get<std::string>();
            movie.title          = item["title"].get<std::string>();
            movie.content_rating = item.value("contentRating", "");
            movie.file_path      = std::move(file_path);
            movie.duration_ms    = item.value("duration", int64_t{0});
            if (item.contains("year") && !item["year"].is_null())
                movie.year = item["year"].get<int>();
            movie.overview  = item.value("summary", "");
            movie.tagline   = item.value("tagline", "");
            movie.studio    = item.value("studio", "");
            movie.thumb     = item.value("thumb", "");
            movie.art       = item.value("art", "");
            if (item.contains("audienceRating") && !item["audienceRating"].is_null())
                movie.audience_rating = item["audienceRating"].get<float>();

            // Director
            if (item.contains("Director") && !item["Director"].empty())
                movie.director = item["Director"][0].value("tag", "");

            // Genres
            json genres = json::array();
            if (item.contains("Genre"))
                for (const auto& g : item["Genre"])
                    genres.push_back(g.value("tag", ""));
            movie.genres = genres.dump();

            // External IDs
            if (item.contains("Guid")) {
                for (const auto& g : item["Guid"]) {
                    std::string id = g.value("id", "");
                    if (id.rfind("imdb://", 0) == 0)  movie.imdb_id = id.substr(7);
                    if (id.rfind("tmdb://", 0) == 0)  movie.tmdb_id = id.substr(7);
                }
            }
            result.push_back(std::move(movie));
        }
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] parse error (movies): " << e.what() << '\n';
    }
    return result;
}

// ---------------------------------------------------------------------------

std::vector<Episode> PlexSource::fetchEpisodes(const std::string& external_show_id) {
    // Builds its own client rather than reusing client_: this runs concurrently
    // across shows during sync, and client_ is not safe to share across threads.
    httplib::Client client(base_url_);
    client.set_default_headers({{"X-Plex-Token", token_}, {"Accept", "application/json"}});
    client.set_connection_timeout(10);
    client.set_read_timeout(30);

    // Request season/episode order explicitly so the Plex server pre-sorts when it can.
    const std::string path = "/library/metadata/" + external_show_id
                           + "/allLeaves?sort=parentIndex%3Aasc%2Cindex%3Aasc";
    auto res = client.Get(path);
    if (!res) {
        std::cerr << "[plex:" << source_id_ << "] /library/metadata/" << external_show_id
                  << "/allLeaves — " << httplib::to_string(res.error()) << '\n';
        return {};
    }
    if (res->status != 200) {
        std::cerr << "[plex:" << source_id_ << "] /library/metadata/" << external_show_id
                  << "/allLeaves — HTTP " << res->status << '\n';
        return {};
    }

    std::vector<Episode> result;
    try {
        auto j = json::parse(res->body);
        for (const auto& item : j["MediaContainer"]["Metadata"]) {
            std::string file_path;
            if (item.contains("Media") && !item["Media"].empty()) {
                const auto& media = item["Media"][0];
                if (media.contains("Part") && !media["Part"].empty())
                    file_path = media["Part"][0].value("file", "");
            }
            if (file_path.empty()) continue;

            const int season = item.value("parentIndex", 0);
            const std::string air_date = item.value("originallyAvailableAt", "");

            // Season 0 (specials/unmatched) with no air date are stub entries —
            // they have no scheduling value and pollute episode cursors.
            if (season == 0 && air_date.empty()) continue;

            Episode ep;
            ep.episode_id  = item["ratingKey"].get<std::string>();
            ep.show_id     = item["grandparentRatingKey"].get<std::string>(); // resolved by SyncManager
            ep.season      = season;
            ep.episode     = item.value("index", 0);
            ep.title       = item.value("title", "");
            ep.file_path   = std::move(file_path);
            ep.duration_ms = item.value("duration", int64_t{0});
            ep.overview    = item.value("summary", "");
            ep.air_date    = air_date;
            ep.thumb       = item.value("thumb", "");
            if (item.contains("absoluteIndex") && !item["absoluteIndex"].is_null())
                ep.absolute_index = item["absoluteIndex"].get<int>();
            result.push_back(std::move(ep));
        }
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] parse error (episodes): " << e.what() << '\n';
    }

    // Sort client-side as a fallback in case the server ignores the sort param.
    std::sort(result.begin(), result.end(), [](const Episode& a, const Episode& b) {
        return a.season != b.season ? a.season < b.season : a.episode < b.episode;
    });

    return result;
}

// ---------------------------------------------------------------------------

std::vector<Playlist> PlexSource::fetchPlaylists(const std::string& /*external_lib_id*/) {
    // TODO: GET /playlists?type=15, then GET /playlists/{id}/items per playlist
    return {};
}
