#include "JellyfinBaseSource.h"
#include "model/Episode.h"
#include "model/Movie.h"
#include "model/Playlist.h"
#include "model/Show.h"
#include <algorithm>
#include <ctime>
#include <iostream>
#include <nlohmann/json.hpp>
#include <unordered_map>

#include "log/DebugLog.h"

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
    // 30s wasn't enough for a heavy field-set (Overview/People/ProviderIds/
    // Tags/...) at Limit=500 against a large or slow self-hosted library —
    // the connection succeeds (Test Connection uses a lightweight ping) but
    // this bulk read times out mid-response, surfacing as httplib::Error::Read.
    client_.set_read_timeout(120);
    // Self-hosted Jellyfin/Emby servers commonly use self-signed or LAN certificates.
    // Follow redirects to handle HTTP→HTTPS forwarding rules.
    client_.enable_server_certificate_verification(false);
    client_.set_follow_location(true);
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

// Parses the leading "YYYY-MM-DDTHH:MM:SS" of a Jellyfin ISO 8601 timestamp
// (e.g. "2024-05-01T12:34:56.7890000Z") to epoch seconds (UTC). Trailing
// fractional seconds / zone suffix are ignored — second resolution is all
// watch-state freshness comparisons need. Returns 0 on parse failure.
int64_t parseIsoToEpoch(const std::string& s) {
    std::tm tm{};
    if (!strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) return 0;
    return static_cast<int64_t>(timegm(&tm));
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
        "ProductionLocations,PremiereDate,Path"
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
        	
        	std::cout << "[jellyfin:" << source_id_ << "] Queried  " << items[0].value("Name", "") << " to " << items[page_count - 1].value("Name", "") << '\n';

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
                show.folder_path    = item.value("Path", "");

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
            	for (const auto& p : pids.items())
            	{
            		show.scraper_info.push_back(ScraperInfo{p.key(), p.value().get<std::string>()});
            	}
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
        "BackdropImageTags,ProductionLocations,PremiereDate,UserData"
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
        	
        	std::cout << "[jellyfin:" << source_id_ << "] Queried  " << items[0].value("Name", "") << " to " << items[page_count - 1].value("Name", "") << '\n';

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

                // Watch state — see Movie.h's src_watched/src_position_ms/src_watched_at.
                if (item.contains("UserData")) {
                    const auto& ud = item["UserData"];
                    if (ud.contains("Played") && !ud["Played"].is_null())
                        movie.src_watched = ud["Played"].get<bool>();
                    if (ud.contains("PlaybackPositionTicks") && !ud["PlaybackPositionTicks"].is_null())
                        movie.src_position_ms = ud["PlaybackPositionTicks"].get<int64_t>() / 10000;
                    if (ud.contains("LastPlayedDate") && !ud["LastPlayedDate"].is_null()) {
                        int64_t epoch = parseIsoToEpoch(ud["LastPlayedDate"].get<std::string>());
                        if (epoch > 0) movie.src_watched_at = epoch;
                    }
                }

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
    client.set_read_timeout(120); // see constructor above for why 30s wasn't enough

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

    // Step 2: episode list — paginated the same way as fetchShows/fetchMovies.
    // Long-running shows (soaps, procedurals, some anime) can have thousands
    // of episodes; a single unpaginated request here was the same class of
    // bug PlexSource::fetchEpisodes had before its /allLeaves paging fix.
    const std::string base_epath =
        "/Shows/" + external_show_id + "/Episodes"
        "?UserId=" + user_id_ +
        "&Fields=Overview,MediaSources,PremiereDate,UserData&EnableTotalRecordCount=false"
        "&Limit=500";

    std::vector<Episode> result;
    int start_index = 0;
    while (true) {
        const std::string epath = base_epath + "&StartIndex=" + std::to_string(start_index);
        auto res = client.Get(epath);
        if (!res) {
            std::cerr << "[" << sourceType() << ":" << source_id_
                      << "] /Shows/" << external_show_id
                      << "/Episodes — " << httplib::to_string(res.error()) << '\n';
            break;
        }
        if (res->status != 200) {
            std::cerr << "[" << sourceType() << ":" << source_id_
                      << "] /Shows/" << external_show_id
                      << "/Episodes — HTTP " << res->status << '\n';
            break;
        }

        int page_count = 0;
        try {
            auto j = json::parse(res->body);
            const auto& items = j["Items"];
            page_count = static_cast<int>(items.size());

            for (const auto& item : items) {
                std::string file_path;
                if (item.contains("MediaSources") && !item["MediaSources"].empty())
                    file_path = item["MediaSources"][0].value("Path", "");
            	if (file_path.empty())
            	{
            		DLOG << item.value("SeriesId", external_show_id) << " - " << item.value("Name", "") << " has no file_path" << "\n";
            		continue;
            	}

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

                // Watch state — see Movie.h's src_watched/src_position_ms/src_watched_at.
                if (item.contains("UserData")) {
                    const auto& ud = item["UserData"];
                    if (ud.contains("Played") && !ud["Played"].is_null())
                        ep.src_watched = ud["Played"].get<bool>();
                    if (ud.contains("PlaybackPositionTicks") && !ud["PlaybackPositionTicks"].is_null())
                        ep.src_position_ms = ud["PlaybackPositionTicks"].get<int64_t>() / 10000;
                    if (ud.contains("LastPlayedDate") && !ud["LastPlayedDate"].is_null()) {
                        int64_t epoch = parseIsoToEpoch(ud["LastPlayedDate"].get<std::string>());
                        if (epoch > 0) ep.src_watched_at = epoch;
                    }
                }

                result.push_back(std::move(ep));
            }
        } catch (const json::exception& e) {
            std::cerr << "[" << sourceType() << ":" << source_id_
                      << "] parse error (episodes " << external_show_id
                      << "): " << e.what() << '\n';
            break;
        }

        if (page_count < 500) break;
        start_index += page_count;
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
// User discovery
// ---------------------------------------------------------------------------

std::vector<SourceUserInfo> JellyfinBaseSource::listServerUsers() {
    // Requires the configured token to have admin rights — same assumption
    // already made for library sync (/Users/{user_id_}/Views etc.).
    auto res = get("/Users");
    if (!res) {
        last_user_discovery_error_ = "/Users — " + httplib::to_string(res.error());
        return {};
    }
    if (res->status != 200) {
        last_user_discovery_error_ = "/Users — HTTP " + std::to_string(res->status);
        return {};
    }

    std::vector<SourceUserInfo> result;
    try {
        auto j = json::parse(res->body);
        for (const auto& u : j) {
            SourceUserInfo info;
            info.external_user_id = u.value("Id", "");
            if (info.external_user_id.empty()) continue;
            info.display_name = u.value("Name", "");
            // Stock Jellyfin/Emby user objects don't carry an email address.
            result.push_back(std::move(info));
        }
        last_user_discovery_error_.clear();
    } catch (const json::exception& e) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] parse error (users): " << e.what() << '\n';
        last_user_discovery_error_ = std::string("/Users — parse error: ") + e.what();
    }
    return result;
}

// ---------------------------------------------------------------------------
// Per-account watch state (for accounts other than the configured primary —
// see source_user.imported_user_id / SyncManager::syncLinkedUserWatchState)
// ---------------------------------------------------------------------------

std::vector<ExternalWatchState> JellyfinBaseSource::fetchWatchState(const std::string& external_user_id) {
    // One sweep of the whole server for this user — Recursive+IncludeItemTypes
    // with no ParentId covers every library in one paginated query, same as
    // fetchMovies()'s pagination shape but without any of its metadata fields:
    // this only ever needs Id/Type/UserData.
    const std::string base_path =
        "/Users/" + external_user_id + "/Items"
        "?IncludeItemTypes=Movie,Episode&Recursive=true&Fields=UserData&Limit=500";

    std::vector<ExternalWatchState> result;
    int start_index = 0;
    while (true) {
        auto res = get(base_path + "&StartIndex=" + std::to_string(start_index));
        if (!res || res->status != 200) break;

        int page_count = 0;
        try {
            auto j = json::parse(res->body);
            const auto& items = j["Items"];
            page_count = static_cast<int>(items.size());

            for (const auto& item : items) {
                if (!item.contains("UserData")) continue;
                const auto& ud = item["UserData"];

                ExternalWatchState st;
                st.external_id = item.value("Id", "");
                if (st.external_id.empty()) continue;
                st.item_type = (item.value("Type", "") == "Movie") ? "movie" : "episode";

                if (ud.contains("Played") && !ud["Played"].is_null())
                    st.watched = ud["Played"].get<bool>();
                if (ud.contains("PlaybackPositionTicks") && !ud["PlaybackPositionTicks"].is_null())
                    st.position_ms = ud["PlaybackPositionTicks"].get<int64_t>() / 10000;
                if (ud.contains("LastPlayedDate") && !ud["LastPlayedDate"].is_null()) {
                    int64_t epoch = parseIsoToEpoch(ud["LastPlayedDate"].get<std::string>());
                    if (epoch > 0) st.watched_at = epoch;
                }

                // Skip items with no actual progress — an untouched item still
                // carries a (mostly-empty) UserData block, and reporting all of
                // them would make every sync pass scan the user's whole library
                // for nothing.
                if (!st.watched.value_or(false) && !st.position_ms.value_or(0)) continue;

                result.push_back(std::move(st));
            }
        } catch (const json::exception& e) {
            std::cerr << "[" << sourceType() << ":" << source_id_
                      << "] parse error (watch state for user " << external_user_id << "): " << e.what() << '\n';
            break;
        }

        if (page_count < 500) break;
        start_index += page_count;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Metadata writeback
// ---------------------------------------------------------------------------

// Jellyfin's update endpoint (POST /Items/{itemId}) takes a full BaseItemDto,
// unlike Plex's per-field query-string PATCH (PlexSource::pushMetadata) — so
// this fetches the existing item, merges in only the changed fields, and
// posts the whole thing back. Scalar text fields only for v1, mirroring
// Plex's own deferral of array fields (genres/actors/countries/collections):
// Jellyfin's merge semantics for those need live verification before it's
// safe to guess at, same reasoning as Plex's multi-value tag convention.
bool JellyfinBaseSource::pushMetadata(const std::string& external_id,
                                       const std::string& /*external_lib_id*/,
                                       const std::string& /*item_type*/,
                                       const WritebackFields& fields) {
    auto getRes = get("/Users/" + user_id_ + "/Items/" + external_id);
    if (!getRes || getRes->status != 200) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] pushMetadata: couldn't fetch existing item (id=" << external_id << ")\n";
        return false;
    }

    json item;
    try {
        item = json::parse(getRes->body);
    } catch (const json::exception& e) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] pushMetadata: parse error (id=" << external_id << "): " << e.what() << '\n';
        return false;
    }

    if (!fields.title.empty())          item["Name"]          = fields.title;
    if (!fields.overview.empty())       item["Overview"]      = fields.overview;
    if (!fields.content_rating.empty()) item["OfficialRating"] = fields.content_rating;
    if (!fields.tagline.empty())        item["Taglines"]      = json::array({fields.tagline});
    if (!fields.studio.empty())         item["Studios"]       = json::array({json{{"Name", fields.studio}}});
    if (!fields.release_date.empty())   item["PremiereDate"]  = fields.release_date + "T00:00:00.0000000Z";

    auto res = client_.Post(("/Items/" + external_id).c_str(), item.dump(), "application/json");
    if (!res) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] pushMetadata failed (id=" << external_id
                  << "): " << httplib::to_string(res.error()) << '\n';
        return false;
    }
    if (res->status != 200 && res->status != 204) {
        std::cerr << "[" << sourceType() << ":" << source_id_
                  << "] pushMetadata HTTP " << res->status << " (id=" << external_id << ")\n";
        return false;
    }
    return true;
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
