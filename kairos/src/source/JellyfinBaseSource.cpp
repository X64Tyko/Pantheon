#include "JellyfinBaseSource.h"
#include "model/Episode.h"
#include "model/Movie.h"
#include "model/Playlist.h"
#include "model/Show.h"
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <unordered_map>

using json = nlohmann::json;

// ---------------------------------------------------------------------------

JellyfinBaseSource::JellyfinBaseSource(
        const std::string& source_id,
        const std::string& base_url,
        const std::string& token,
        const std::string& user_id,
        const std::string& auth_header)
    : source_id_(source_id), base_url_(base_url)
    , token_(token), user_id_(user_id), auth_header_(auth_header)
    , client_(base_url)
{
    client_.set_default_headers({{auth_header_, token_}, {"Accept", "application/json"}});
    client_.set_connection_timeout(10);
    client_.set_read_timeout(30);
}

httplib::Result JellyfinBaseSource::get(const std::string& path) {
    auto res = client_.Get(path);
    if (!res)
        std::cerr << "[" << sourceType() << ":" << source_id_ << "] "
                  << path << " — " << httplib::to_string(res.error()) << '\n';
    else if (res->status != 200)
        std::cerr << "[" << sourceType() << ":" << source_id_ << "] "
                  << path << " — HTTP " << res->status << '\n';
    return res;
}

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------

namespace {

// Trims an ISO 8601 timestamp to "YYYY-MM-DD".
std::string isoDate(const std::string& s) {
    return (s.size() >= 10) ? s.substr(0, 10) : s;
}

// Builds a JSON-array string from a JSON array of strings.
std::string jsonStringArray(const json& arr) {
    json out = json::array();
    for (const auto& v : arr)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out.dump();
}

// Relative image path for the Primary thumbnail.
std::string thumbPath(const std::string& item_id, const json& item) {
    if (item.contains("ImageTags") && item["ImageTags"].contains("Primary"))
        return "/Items/" + item_id + "/Images/Primary";
    return "";
}

// Relative image path for the first backdrop.
std::string artPath(const std::string& item_id, const json& item) {
    if (item.contains("BackdropImageTags") && !item["BackdropImageTags"].empty())
        return "/Items/" + item_id + "/Images/Backdrop";
    return "";
}

// Name of the first person in People array with the given role type.
std::string firstPerson(const json& people, const std::string& role) {
    for (const auto& p : people)
        if (p.value("Type", "") == role)
            return p.value("Name", "");
    return "";
}

// JSON array of names for everyone with the given role.
std::string personNames(const json& people, const std::string& role) {
    json out = json::array();
    for (const auto& p : people)
        if (p.value("Type", "") == role)
            out.push_back(p.value("Name", ""));
    return out.dump();
}

} // namespace

// ---------------------------------------------------------------------------
// Library discovery
// ---------------------------------------------------------------------------

std::vector<LibraryInfo> JellyfinBaseSource::listAvailableLibraries() {
    auto res = get("/Users/" + user_id_ + "/Views");
    if (!res || res->status != 200) return {};

    std::vector<LibraryInfo> result;
    try {
        auto j = json::parse(res->body);
        for (const auto& item : j["Items"]) {
            const std::string ct = item.value("CollectionType", "");
            LibraryInfo info;
            info.external_lib_id = item["Id"].get<std::string>();
            info.name            = item.value("Name", "");
            if      (ct == "tvshows")                            info.type = "show";
            else if (ct == "movies")                             info.type = "movie";
            else if (ct == "music")                              info.type = "music";
            else if (ct == "photos")                             info.type = "photo";
            else if (ct == "homevideos" || ct == "musicvideos")  info.type = "mixed";
            else                                                  info.type = "mixed";
            std::cout << "[" << sourceType() << ":" << source_id_
                      << "] found library: \"" << info.name
                      << "\" collectionType=" << (ct.empty() ? "(none)" : ct)
                      << " → suggested type: " << info.type << '\n';
            result.push_back(std::move(info));
        }
    } catch (const json::exception& e) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] parse error (views): " << e.what() << '\n';
    }
    return result;
}

// ---------------------------------------------------------------------------
// Shows
// ---------------------------------------------------------------------------

std::vector<Show> JellyfinBaseSource::fetchShows(const std::string& external_lib_id) {
    const std::string base_path =
        "/Users/" + user_id_ + "/Items"
        "?ParentId=" + external_lib_id +
        "&IncludeItemTypes=Series&Recursive=true"
        "&Fields=Overview,Genres,Studios,People,ProviderIds,Tags,ProductionYear,"
        "OfficialRating,CommunityRating,Status,ImageTags,BackdropImageTags,"
        "ProductionLocations,PremiereDate"
        "&Limit=500";

    std::vector<Show> result;
    int start_index = 0;

    while (true) {
        const std::string path = base_path + "&StartIndex=" + std::to_string(start_index);
        auto res = get(path);
        if (!res || res->status != 200) break;

        int page_count = 0;
        try {
            auto j = json::parse(res->body);
            const auto& items = j["Items"];
            page_count = static_cast<int>(items.size());

            for (const auto& item : items) {
                const std::string id = item["Id"].get<std::string>();
                Show show;
                show.show_id        = id;
                show.title          = item.value("Name", "");
                show.content_rating = item.value("OfficialRating", "");
                show.overview       = item.value("Overview", "");
                show.status         = item.value("Status", "");
                show.thumb          = thumbPath(id, item);
                show.art            = artPath(id, item);

                // In Jellyfin, Studios holds the network name for TV shows.
                if (item.contains("Studios") && !item["Studios"].empty()) {
                    show.studio  = item["Studios"][0].value("Name", "");
                    show.network = show.studio;
                }

                if (item.contains("ProductionYear") && !item["ProductionYear"].is_null())
                    show.year = item["ProductionYear"].get<int>();
                if (item.contains("CommunityRating") && !item["CommunityRating"].is_null())
                    show.audience_rating = item["CommunityRating"].get<float>();
                if (item.contains("PremiereDate") && !item["PremiereDate"].is_null())
                    show.originally_available_at = isoDate(item["PremiereDate"].get<std::string>());

                if (item.contains("Genres"))
                    show.genres   = jsonStringArray(item["Genres"]);
                if (item.contains("Tags"))
                    show.labels   = jsonStringArray(item["Tags"]);
                if (item.contains("ProductionLocations"))
                    show.countries = jsonStringArray(item["ProductionLocations"]);
                if (item.contains("People"))
                    show.actors   = personNames(item["People"], "Actor");

                const auto& pids = item.value("ProviderIds", json::object());
                show.imdb_id = pids.value("Imdb", "");
                show.tvdb_id = pids.value("Tvdb", "");
                show.tmdb_id = pids.value("Tmdb", "");

                result.push_back(std::move(show));
            }
        } catch (const json::exception& e) {
            std::cerr << "[" << sourceType() << ":" << source_id_
                      << "] parse error (shows): " << e.what() << '\n';
            break;
        }

        if (page_count < 500) break;
        start_index += page_count;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Movies
// ---------------------------------------------------------------------------

std::vector<Movie> JellyfinBaseSource::fetchMovies(const std::string& external_lib_id) {
    const std::string base_path =
        "/Users/" + user_id_ + "/Items"
        "?ParentId=" + external_lib_id +
        "&IncludeItemTypes=Movie&Recursive=true"
        "&Fields=Overview,Genres,Studios,People,ProviderIds,Tags,ProductionYear,"
        "OfficialRating,CommunityRating,Tagline,MediaSources,ImageTags,"
        "BackdropImageTags,ProductionLocations,PremiereDate"
        "&Limit=500";

    std::vector<Movie> result;
    int start_index = 0;

    while (true) {
        const std::string path = base_path + "&StartIndex=" + std::to_string(start_index);
        auto res = get(path);
        if (!res || res->status != 200) break;

        int page_count = 0;
        try {
            auto j = json::parse(res->body);
            const auto& items = j["Items"];
            page_count = static_cast<int>(items.size());

            for (const auto& item : items) {
                std::string file_path;
                if (item.contains("MediaSources") && !item["MediaSources"].empty())
                    file_path = item["MediaSources"][0].value("Path", "");
                if (file_path.empty()) continue;

                const std::string id = item["Id"].get<std::string>();
                Movie movie;
                movie.movie_id       = id;
                movie.title          = item.value("Name", "");
                movie.content_rating = item.value("OfficialRating", "");
                movie.file_path      = std::move(file_path);
                movie.overview       = item.value("Overview", "");
                movie.tagline        = item.value("Tagline", "");
                movie.thumb          = thumbPath(id, item);
                movie.art            = artPath(id, item);

                // RunTimeTicks is 100ns units; divide by 10000 for ms.
                {
                    int64_t ticks = 0;
                    if (item.contains("RunTimeTicks") && !item["RunTimeTicks"].is_null())
                        ticks = item["RunTimeTicks"].get<int64_t>();
                    if (ticks <= 0 && item.contains("MediaSources") && !item["MediaSources"].empty())
                        ticks = item["MediaSources"][0].value("RunTimeTicks", int64_t{0});
                    movie.duration_ms = ticks / 10000;
                }

                if (item.contains("ProductionYear") && !item["ProductionYear"].is_null())
                    movie.year = item["ProductionYear"].get<int>();
                if (item.contains("CommunityRating") && !item["CommunityRating"].is_null())
                    movie.audience_rating = item["CommunityRating"].get<float>();

                if (item.contains("Studios") && !item["Studios"].empty())
                    movie.studio = item["Studios"][0].value("Name", "");

                if (item.contains("People")) {
                    movie.director = firstPerson(item["People"], "Director");
                    movie.actors   = personNames(item["People"], "Actor");
                }

                if (item.contains("Genres"))
                    movie.genres   = jsonStringArray(item["Genres"]);
                if (item.contains("Tags"))
                    movie.labels   = jsonStringArray(item["Tags"]);
                if (item.contains("ProductionLocations"))
                    movie.countries = jsonStringArray(item["ProductionLocations"]);

                const auto& pids = item.value("ProviderIds", json::object());
                movie.imdb_id = pids.value("Imdb", "");
                movie.tmdb_id = pids.value("Tmdb", "");

                result.push_back(std::move(movie));
            }
        } catch (const json::exception& e) {
            std::cerr << "[" << sourceType() << ":" << source_id_
                      << "] parse error (movies): " << e.what() << '\n';
            break;
        }

        if (page_count < 500) break;
        start_index += page_count;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Episodes — builds its own client each call: fetchEpisodes runs concurrently
// across shows during sync, and client_ is not safe to share across threads.
// ---------------------------------------------------------------------------

std::vector<Episode> JellyfinBaseSource::fetchEpisodes(const std::string& external_show_id) {
    httplib::Client client(base_url_);
    client.set_default_headers({{auth_header_, token_}, {"Accept", "application/json"}});
    client.set_connection_timeout(10);
    client.set_read_timeout(30);

    // Step 1: season names (IndexNumber → Name) for season_name population.
    std::unordered_map<int, std::string> season_names;
    {
        const std::string spath =
            "/Shows/" + external_show_id + "/Seasons"
            "?UserId=" + user_id_;
        auto sr = client.Get(spath);
        if (sr && sr->status == 200) {
            try {
                auto j = json::parse(sr->body);
                for (const auto& s : j["Items"])
                    if (s.contains("IndexNumber") && !s["IndexNumber"].is_null())
                        season_names[s["IndexNumber"].get<int>()] = s.value("Name", "");
            } catch (...) {}
        }
    }

    // Step 2: episode list.
    const std::string epath =
        "/Shows/" + external_show_id + "/Episodes"
        "?UserId=" + user_id_ +
        "&Fields=Overview,MediaSources,PremiereDate&EnableTotalRecordCount=false";
    auto res = client.Get(epath);
    if (!res) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] /Shows/" << external_show_id
                  << "/Episodes — " << httplib::to_string(res.error()) << '\n';
        return {};
    }
    if (res->status != 200) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] /Shows/" << external_show_id
                  << "/Episodes — HTTP " << res->status << '\n';
        return {};
    }

    std::vector<Episode> result;
    try {
        auto j = json::parse(res->body);
        for (const auto& item : j["Items"]) {
            std::string file_path;
            if (item.contains("MediaSources") && !item["MediaSources"].empty())
                file_path = item["MediaSources"][0].value("Path", "");
            if (file_path.empty()) continue;

            const std::string id = item["Id"].get<std::string>();
            const int season     = item.value("ParentIndexNumber", 0);

            Episode ep;
            ep.episode_id  = id;
            ep.show_id     = item.value("SeriesId", external_show_id);
            ep.season      = season;
            ep.episode     = item.value("IndexNumber", 0);
            ep.title       = item.value("Name", "");
            ep.file_path   = std::move(file_path);
            ep.overview    = item.value("Overview", "");
            ep.thumb       = thumbPath(id, item);

            if (item.contains("PremiereDate") && !item["PremiereDate"].is_null())
                ep.air_date = isoDate(item["PremiereDate"].get<std::string>());

            {
                int64_t ticks = 0;
                if (item.contains("RunTimeTicks") && !item["RunTimeTicks"].is_null())
                    ticks = item["RunTimeTicks"].get<int64_t>();
                if (ticks <= 0 && item.contains("MediaSources") && !item["MediaSources"].empty())
                    ticks = item["MediaSources"][0].value("RunTimeTicks", int64_t{0});
                ep.duration_ms = ticks / 10000;
            }

            if (item.contains("AbsoluteEpisodeNumber") && !item["AbsoluteEpisodeNumber"].is_null())
                ep.absolute_index = item["AbsoluteEpisodeNumber"].get<int>();

            auto sn_it = season_names.find(season);
            if (sn_it != season_names.end())
                ep.season_name = sn_it->second;

            result.push_back(std::move(ep));
        }
    } catch (const json::exception& e) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] parse error (episodes " << external_show_id
                  << "): " << e.what() << '\n';
    }

    std::sort(result.begin(), result.end(), [](const Episode& a, const Episode& b) {
        return a.season != b.season ? a.season < b.season : a.episode < b.episode;
    });
    return result;
}

// ---------------------------------------------------------------------------
// Playlists sync — deferred; browse API covers the live-query use case.
// ---------------------------------------------------------------------------

std::vector<Playlist> JellyfinBaseSource::fetchPlaylists(const std::string&) {
    return {};
}

// ---------------------------------------------------------------------------
// Browse — playlists
// ---------------------------------------------------------------------------

std::vector<BrowseListItem> JellyfinBaseSource::browsePlaylists() {
    const std::string path =
        "/Users/" + user_id_ + "/Items"
        "?IncludeItemTypes=Playlist&Recursive=true&Fields=ChildCount";
    auto res = get(path);
    if (!res || res->status != 200) return {};

    std::vector<BrowseListItem> result;
    try {
        auto j = json::parse(res->body);
        for (const auto& item : j["Items"])
            result.push_back({item["Id"].get<std::string>(),
                              item.value("Name", ""),
                              item.value("ChildCount", 0)});
    } catch (const json::exception& e) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] parse error (browse playlists): " << e.what() << '\n';
    }
    return result;
}

std::vector<BrowseContentItem> JellyfinBaseSource::browsePlaylistItems(const std::string& id) {
    const std::string path =
        "/Playlists/" + id + "/Items?UserId=" + user_id_ + "&Fields=Overview,MediaSources";
    auto res = get(path);
    if (!res || res->status != 200) return {};

    std::vector<BrowseContentItem> result;
    try {
        auto j = json::parse(res->body);
        for (const auto& item : j["Items"]) {
            const std::string type = item.value("Type", "");
            BrowseContentItem entry;
            entry.external_id = item["Id"].get<std::string>();
            entry.item_type   = (type == "Movie") ? "movie" : "episode";
            entry.title       = item.value("Name", "");
            entry.duration_ms =
                item.value("RunTimeTicks", int64_t{0}) / 10000;
            if (type == "Episode") {
                entry.show_title = item.value("SeriesName", "");
                entry.season     = item.value("ParentIndexNumber", -1);
                entry.episode    = item.value("IndexNumber", -1);
            }
            result.push_back(std::move(entry));
        }
    } catch (const json::exception& e) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] parse error (browse playlist items): " << e.what() << '\n';
    }
    return result;
}

// ---------------------------------------------------------------------------
// Browse — collections (BoxSets)
// ---------------------------------------------------------------------------

std::vector<BrowseListItem> JellyfinBaseSource::browseCollections(const std::string& ext_lib_id) {
    std::string path =
        "/Users/" + user_id_ + "/Items"
        "?IncludeItemTypes=BoxSet&Recursive=true&Fields=ChildCount";
    if (!ext_lib_id.empty())
        path += "&ParentId=" + ext_lib_id;
    auto res = get(path);
    if (!res || res->status != 200) return {};

    std::vector<BrowseListItem> result;
    try {
        auto j = json::parse(res->body);
        for (const auto& item : j["Items"])
            result.push_back({item["Id"].get<std::string>(),
                              item.value("Name", ""),
                              item.value("ChildCount", 0)});
    } catch (const json::exception& e) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] parse error (browse collections): " << e.what() << '\n';
    }
    return result;
}

std::vector<BrowseContentItem> JellyfinBaseSource::browseCollectionItems(const std::string& id) {
    const std::string path =
        "/Users/" + user_id_ + "/Items"
        "?ParentId=" + id +
        "&Recursive=true&IncludeItemTypes=Movie,Episode&Fields=Overview,MediaSources";
    auto res = get(path);
    if (!res || res->status != 200) return {};

    std::vector<BrowseContentItem> result;
    try {
        auto j = json::parse(res->body);
        for (const auto& item : j["Items"]) {
            const std::string type = item.value("Type", "");
            BrowseContentItem entry;
            entry.external_id = item["Id"].get<std::string>();
            entry.item_type   = (type == "Movie") ? "movie" : "episode";
            entry.title       = item.value("Name", "");
            entry.duration_ms =
                item.value("RunTimeTicks", int64_t{0}) / 10000;
            if (type == "Episode") {
                entry.show_title = item.value("SeriesName", "");
                entry.season     = item.value("ParentIndexNumber", -1);
                entry.episode    = item.value("IndexNumber", -1);
            }
            result.push_back(std::move(entry));
        }
    } catch (const json::exception& e) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] parse error (browse collection items): " << e.what() << '\n';
    }
    return result;
}
