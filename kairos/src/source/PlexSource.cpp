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
            const std::string plex_type = dir.value("type", "");
            LibraryInfo info;
            info.external_lib_id = dir["key"].get<std::string>();
            info.name            = dir["title"].get<std::string>();
            if      (plex_type == "show")   info.type = "show";
            else if (plex_type == "movie")  info.type = "movie";
            else if (plex_type == "artist") info.type = "music";
            else if (plex_type == "photo")  info.type = "photo";
            else                            info.type = "mixed";
            std::cout << "[plex:" << source_id_ << "] found library: \""
                      << info.name << "\" type=" << plex_type
                      << " → suggested type: " << info.type << '\n';
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

            // Labels
            json labels = json::array();
            if (item.contains("Label"))
                for (const auto& l : item["Label"])
                    labels.push_back(l.value("tag", ""));
            show.labels = labels.dump();

            // Network (Plex exposes it as an array)
            if (item.contains("Network") && !item["Network"].empty())
                show.network = item["Network"][0].value("tag", "");

            // Actors (Role array)
            json actors = json::array();
            if (item.contains("Role"))
                for (const auto& r : item["Role"])
                    actors.push_back(r.value("tag", ""));
            show.actors = actors.dump();

            // Countries
            json countries = json::array();
            if (item.contains("Country"))
                for (const auto& c : item["Country"])
                    countries.push_back(c.value("tag", ""));
            show.countries = countries.dump();

            // Collections
            json collections = json::array();
            if (item.contains("Collection"))
                for (const auto& c : item["Collection"])
                    collections.push_back(c.value("tag", ""));
            show.collections = collections.dump();

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
            {
                int64_t dur = item.value("duration", int64_t{0});
                if (dur <= 0 && item.contains("Media") && !item["Media"].empty())
                    dur = item["Media"][0].value("duration", int64_t{0});
                movie.duration_ms = dur;
            }
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

            // Labels
            json labels = json::array();
            if (item.contains("Label"))
                for (const auto& l : item["Label"])
                    labels.push_back(l.value("tag", ""));
            movie.labels = labels.dump();

            // Actors (Role array)
            json actors = json::array();
            if (item.contains("Role"))
                for (const auto& r : item["Role"])
                    actors.push_back(r.value("tag", ""));
            movie.actors = actors.dump();

            // Countries
            json countries = json::array();
            if (item.contains("Country"))
                for (const auto& c : item["Country"])
                    countries.push_back(c.value("tag", ""));
            movie.countries = countries.dump();

            // Collections
            json collections = json::array();
            if (item.contains("Collection"))
                for (const auto& c : item["Collection"])
                    collections.push_back(c.value("tag", ""));
            movie.collections = collections.dump();

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
            const std::string season_name = item.value("parentTitle", "");
            const std::string air_date = item.value("originallyAvailableAt", "");

            Episode ep;
            ep.episode_id  = item["ratingKey"].get<std::string>();
            ep.show_id     = item["grandparentRatingKey"].get<std::string>(); // resolved by SyncManager
            ep.season      = season;
            ep.season_name = season_name;
            ep.episode     = item.value("index", 0);
            ep.title       = item.value("title", "");
            ep.file_path   = std::move(file_path);
            ep.duration_ms = item.value("duration", int64_t{0});
            // Plex sometimes omits top-level duration for episodes; fall back to
            // the Media-level value the way fetchMovies already does.
            if (ep.duration_ms <= 0 && item.contains("Media") && !item["Media"].empty())
                ep.duration_ms = item["Media"][0].value("duration", int64_t{0});
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

// ---------------------------------------------------------------------------
// Browse — live queries, not synced data
// ---------------------------------------------------------------------------

// Shared parser for playlist/collection item responses.
// Returns BrowseContentItems with external_id populated; kairos_id resolution
// is left to the Router (DB lookup against source_mapping).
static std::vector<BrowseContentItem> parseBrowseItems(const std::string& body) {
    std::vector<BrowseContentItem> result;
    try {
        auto j = json::parse(body);
        const auto& md = j["MediaContainer"];
        if (!md.contains("Metadata")) return result;
        for (const auto& item : md["Metadata"]) {
            std::string plex_type = item.value("type", "");
            BrowseContentItem entry;
            entry.external_id  = item["ratingKey"].get<std::string>();
            entry.item_type    = (plex_type == "movie") ? "movie" : "episode";
            entry.title        = item.value("title", "");
            entry.duration_ms  = item.value("duration", int64_t{0});
            if (plex_type == "episode") {
                entry.show_title = item.value("grandparentTitle", "");
                if (item.contains("parentIndex") && !item["parentIndex"].is_null())
                    entry.season  = item["parentIndex"].get<int>();
                if (item.contains("index") && !item["index"].is_null())
                    entry.episode = item["index"].get<int>();
            }
            result.push_back(std::move(entry));
        }
    } catch (...) {}
    return result;
}

std::vector<BrowseListItem> PlexSource::browsePlaylists() {
    auto res = get("/playlists?playlistType=video");
    if (!res || res->status != 200) return {};
    std::vector<BrowseListItem> result;
    try {
        auto j = json::parse(res->body);
        const auto& md = j["MediaContainer"];
        if (md.contains("Metadata")) {
            for (const auto& pl : md["Metadata"]) {
                result.push_back({
                    pl["ratingKey"].get<std::string>(),
                    pl.value("title", ""),
                    pl.value("leafCount", 0),
                });
            }
        }
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] parse error (browse playlists): " << e.what() << '\n';
    }
    return result;
}

std::vector<BrowseContentItem> PlexSource::browsePlaylistItems(const std::string& id) {
    auto res = get("/playlists/" + id + "/items");
    if (!res || res->status != 200) return {};
    return parseBrowseItems(res->body);
}

std::vector<BrowseListItem> PlexSource::browseCollections(const std::string& ext_lib_id) {
    auto res = get("/library/sections/" + ext_lib_id + "/collections");
    if (!res || res->status != 200) return {};
    std::vector<BrowseListItem> result;
    try {
        auto j = json::parse(res->body);
        const auto& md = j["MediaContainer"];
        if (md.contains("Metadata")) {
            for (const auto& col : md["Metadata"]) {
                result.push_back({
                    col["ratingKey"].get<std::string>(),
                    col.value("title", ""),
                    col.value("childCount", 0),
                });
            }
        }
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] parse error (browse collections): " << e.what() << '\n';
    }
    return result;
}

std::vector<BrowseContentItem> PlexSource::browseCollectionItems(const std::string& id) {
    auto res = get("/library/metadata/" + id + "/children");
    if (!res || res->status != 200) return {};
    return parseBrowseItems(res->body);
}

std::optional<std::vector<PlexListItem>> PlexSource::fetchListItems(
    const std::string& id, const std::string& plex_type) {
    std::string path = (plex_type == "playlist")
        ? "/playlists/" + id + "/items"
        : "/library/metadata/" + id + "/children";
    auto res = get(path);
    if (!res || res->status != 200) return std::nullopt;
    try {
        auto raw = parseBrowseItems(res->body);
        std::vector<PlexListItem> result;
        result.reserve(raw.size());
        for (auto& item : raw)
            result.push_back({item.item_type, item.external_id});
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------

std::vector<Chapter> PlexSource::fetchIntroMarkers(const std::string& external_id) {
    auto res = get("/library/metadata/" + external_id + "/markers");
    if (!res || res->status != 200) return {};

    std::vector<Chapter> result;
    try {
        auto j = json::parse(res->body);
        const auto& mc = j["MediaContainer"];
        if (!mc.contains("Marker")) return result;
        int pos = 0;
        for (const auto& m : mc["Marker"]) {
            Chapter c;
            c.source   = "plex_intro";
            c.position = pos++;
            const std::string type = m.value("type", std::string("intro"));
            c.chapter_type = (type == "credits") ? "credits" : "intro";
            c.start_ms = m.value("startTimeOffset", int64_t{0});
            c.end_ms   = m.value("endTimeOffset",   int64_t{0});
            result.push_back(std::move(c));
        }
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] marker parse error (id=" << external_id
                  << "): " << e.what() << '\n';
    }
    return result;
}

std::vector<Chapter> PlexSource::fetchChapters(const std::string& external_id) {
    auto res = get("/library/metadata/" + external_id + "?includeChapters=1");
    if (!res || res->status != 200) return {};

    std::vector<Chapter> result;
    try {
        auto j = json::parse(res->body);
        const auto& mc = j["MediaContainer"];
        if (!mc.contains("Metadata") || mc["Metadata"].empty()) return result;
        const auto& meta = mc["Metadata"][0];
        if (!meta.contains("Chapter")) return result;
        int pos = 0;
        for (const auto& ch : meta["Chapter"]) {
            Chapter c;
            c.source       = "plex_chapters";
            c.chapter_type = "unclassified";
            c.position     = pos++;
            c.title        = ch.value("tag", std::string(""));
            c.start_ms     = ch.value("startTimeOffset", int64_t{0});
            c.end_ms       = ch.value("endTimeOffset",   int64_t{0});
            result.push_back(std::move(c));
        }
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] chapter parse error (id=" << external_id
                  << "): " << e.what() << '\n';
    }
    return result;
}
