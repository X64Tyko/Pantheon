#include "PlexSource.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <iostream>
#include <unordered_map>

#include "log/DebugLog.h"

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
    // 30s wasn't enough for a bulk listing (e.g. /allLeaves) against a large
    // or slow library — connection succeeds fast, but the full read can run
    // long, surfacing as httplib::Error::Read ("Failed to read connection").
    client_.set_read_timeout(120);
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
    // Paginated (see fetchMovies for why) — a single request for the whole
    // section is what forced the read timeout up to 120s in the first place,
    // and large enough libraries can still exceed that in one shot.
    constexpr int kPageSize = 200;
    std::vector<Show> result;
    int start = 0;
    while (true) {
        const std::string path = "/library/sections/" + external_lib_id + "/all?type=2"
    		"&sort=addedAt:asc"
            "&X-Plex-Container-Start=" + std::to_string(start) +
            "&X-Plex-Container-Size=" + std::to_string(kPageSize);
        auto res = get(path);
        if (!res || res->status != 200) break;

        int page_count = 0;
        try {
            auto j = json::parse(res->body);
            if (!j["MediaContainer"].contains("Metadata")) break;
            const auto& items = j["MediaContainer"]["Metadata"];
            page_count = static_cast<int>(items.size());
        	std::cout << "[plex:" << source_id_ << "] Queried  " << items[0]["title"] << " to " << items[page_count - 1]["title"] << '\n';
            result.reserve(result.size() + page_count);
            for (const auto& minItem : items) {
            	std::string detail_path = "/library/metadata/" + minItem["ratingKey"].get<std::string>();
            	auto details = get(detail_path);
            	if (!details || details->status != 200) break;
            	if (!details->body.empty())
            	{
             		auto d = json::parse(details->body);
       //      		std::cout << "[plex:" << source_id_ << "] Recieved Data: "
					  // << to_string(d) << '\n';
            		
            		auto item = d["MediaContainer"]["Metadata"][0];
            		Show show;
            		show.show_id        = item["ratingKey"].get<std::string>();
            		show.title          = item["title"].get<std::string>();
            		show.original_title = item.value("originalTitle", "");
            		show.content_rating = item.value("contentRating", "");
            		show.overview       = item.value("summary", "");
            		show.tagline        = item.value("tagline", "");
            		show.studio         = item.value("studio", "");
            		show.status         = item.value("status", "");
            		show.thumb          = item.value("thumb", "");
            		show.art            = item.value("art", "");
            		show.originally_available_at = item.value("originallyAvailableAt", "");
            		show.added_at       = item.value("addedAt", 0);
            		show.added_at_source = source_id_;

            		// Root folder(s): [{"path":"/data/TV Shows/Show Name"}, ...] —
            		// same array-of-object shape as Genre/Label below. Only the
            		// first entry is used (multi-location shows are rare and the
            		// dedup use of this field only needs "a" folder, not all of
            		// them). NOTE: this field's exact shape hasn't been verified
            		// against a live Plex server in this change — confirm before
            		// relying on it, and adjust the key name if it differs.
            		if (item.contains("Location") && !item["Location"].empty())
            			show.folder_path = item["Location"][0].value("path", "");
            		if (item.contains("year") && !item["year"].is_null())
            			show.year = item["year"].get<int>();
            		if (item.contains("audienceRating") && !item["audienceRating"].is_null())
            			show.audience_rating = item["audienceRating"].get<float>();

            		if (item.contains("Rating") && !item["Rating"].empty())
            		{
            			for (const auto& r : item["Rating"])
            				show.ratings.push_back(Rating{r["image"], r["type"], r["value"]});
            		}
            		
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
            				int titleEnd = id.find(':');
            				if (titleEnd != std::string::npos)
            					show.scraper_info.push_back(ScraperInfo{id.substr(0, titleEnd), id.substr(titleEnd + 3)});
            				if (id.rfind("imdb://", 0) == 0)  show.imdb_id = id.substr(7);
            				if (id.rfind("tvdb://", 0) == 0)  show.tvdb_id = id.substr(7);
            				if (id.rfind("tmdb://", 0) == 0)  show.tmdb_id = id.substr(7);
            			}
            		}
            		result.push_back(std::move(show));
            	}
            }
        } catch (const json::exception& e) {
            std::cerr << "[plex:" << source_id_ << "] parse error (shows): " << e.what() << '\n';
            break;
        }

        if (page_count < kPageSize) break;
        start += page_count;
    }
    return result;
}

// ---------------------------------------------------------------------------

std::vector<Movie> PlexSource::fetchMovies(const std::string& external_lib_id) {
    // Paginated with X-Plex-Container-Start/Size — a single request for the
    // whole section is what forced the read timeout up to 120s (see the
    // client_ setup in the constructor), and large enough libraries (or slow
    // servers building the response) can still exceed that in one shot.
    constexpr int kPageSize = 200;
    std::vector<Movie> result;
    int start = 0;
    while (true) {
        const std::string path = "/library/sections/" + external_lib_id + "/all?type=1"
            "&X-Plex-Container-Start=" + std::to_string(start) +
            "&X-Plex-Container-Size=" + std::to_string(kPageSize);
        auto res = get(path);
        if (!res || res->status != 200) break;

        int page_count = 0;
        try {
            auto j = json::parse(res->body);
            if (!j["MediaContainer"].contains("Metadata")) break;
            const auto& items = j["MediaContainer"]["Metadata"];
            page_count = static_cast<int>(items.size());
        	
        	std::cout << "[plex:" << source_id_ << "] Queried  " << items[0]["title"] << " to " << items[page_count - 1]["title"] << '\n';
        	
            result.reserve(result.size() + page_count);
            for (const auto& item : items) {
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
                movie.original_title = item.value("originalTitle", "");
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
                movie.added_at        = item.value("addedAt", int64_t{0});
                movie.added_at_source = source_id_;

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

                // Watch state — see Movie.h's src_watched/src_position_ms/src_watched_at/src_view_count.
                if (item.contains("viewCount") && !item["viewCount"].is_null()) {
                    movie.src_view_count = item["viewCount"].get<int64_t>();
                    movie.src_watched    = *movie.src_view_count > 0;
                }
                if (item.contains("viewOffset") && !item["viewOffset"].is_null())
                    movie.src_position_ms = item["viewOffset"].get<int64_t>();
                if (item.contains("lastViewedAt") && !item["lastViewedAt"].is_null())
                    movie.src_watched_at = item["lastViewedAt"].get<int64_t>();

                result.push_back(std::move(movie));
            }
        } catch (const json::exception& e) {
            std::cerr << "[plex:" << source_id_ << "] parse error (movies): " << e.what() << '\n';
            break;
        }

        if (page_count < kPageSize) break;
        start += page_count;
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
    client.set_read_timeout(120); // see constructor above for why 30s wasn't enough

    // Paginated — this is the "bulk listing" the 120s read timeout above was
    // originally added for; long-running shows (soaps, procedurals, some
    // anime) can have thousands of episodes, and a single /allLeaves request
    // for all of them can still exceed even that.
    constexpr int kPageSize = 200;
    std::vector<Episode> result;
    int start = 0;
    while (true) {
        // Request season/episode order explicitly so the Plex server pre-sorts when it can.
        const std::string path = "/library/metadata/" + external_show_id
            + "/allLeaves?sort=parentIndex%3Aasc%2Cindex%3Aasc"
              "&X-Plex-Container-Start=" + std::to_string(start) +
              "&X-Plex-Container-Size=" + std::to_string(kPageSize);
        auto res = client.Get(path);
        if (!res) {
            std::cerr << "[plex:" << source_id_ << "] /library/metadata/" << external_show_id
                      << "/allLeaves — " << httplib::to_string(res.error()) << '\n';
            break;
        }
        if (res->status != 200) {
            std::cerr << "[plex:" << source_id_ << "] /library/metadata/" << external_show_id
                      << "/allLeaves — HTTP " << res->status << '\n';
            break;
        }

        int page_count = 0;
        try {
            auto j = json::parse(res->body);
            if (!j["MediaContainer"].contains("Metadata")) break;
            const auto& items = j["MediaContainer"]["Metadata"];
            page_count = static_cast<int>(items.size());
            result.reserve(result.size() + page_count);
            for (const auto& item : items) {
                std::string file_path;
                if (item.contains("Media") && !item["Media"].empty()) {
                    const auto& media = item["Media"][0];
                    if (media.contains("Part") && !media["Part"].empty())
                        file_path = media["Part"][0].value("file", "");
                }
                if (file_path.empty())
                {
                	DLOG << item["grandparentTitle"].get<std::string>() << " - " << item["title"].get<std::string>() << " has no file_path" << "\n";
	                continue;
                }

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

                // Watch state — see Movie.h's src_watched/src_position_ms/src_watched_at/src_view_count.
                if (item.contains("viewCount") && !item["viewCount"].is_null()) {
                    ep.src_view_count = item["viewCount"].get<int64_t>();
                    ep.src_watched    = *ep.src_view_count > 0;
                }
                if (item.contains("viewOffset") && !item["viewOffset"].is_null())
                    ep.src_position_ms = item["viewOffset"].get<int64_t>();
                if (item.contains("lastViewedAt") && !item["lastViewedAt"].is_null())
                    ep.src_watched_at = item["lastViewedAt"].get<int64_t>();

                result.push_back(std::move(ep));
            }
        } catch (const json::exception& e) {
            std::cerr << "[plex:" << source_id_ << "] parse error (episodes): " << e.what() << '\n';
            break;
        }

        if (page_count < kPageSize) break;
        start += page_count;
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
// is left to the Router (DB lookup against source_mapping). Each item is
// parsed independently — one malformed entry (e.g. missing ratingKey) used
// to be caught by one try/catch wrapping the whole loop, which silently
// discarded every item after the bad one instead of just that one.
static std::vector<BrowseContentItem> parseBrowseItems(const std::string& body) {
    std::vector<BrowseContentItem> result;
    json j;
    try { j = json::parse(body); } catch (...) { return result; }
    if (!j.contains("MediaContainer")) return result;
    const auto& md = j["MediaContainer"];
    if (!md.contains("Metadata")) return result;
    for (const auto& item : md["Metadata"]) {
        try {
            // .value(), not operator[] — operator[] on a *const* json object
            // asserts (aborts, uncatchably) rather than throwing when the key
            // is missing, which would defeat this try/catch entirely.
            std::string rating_key = item.value("ratingKey", "");
            if (rating_key.empty()) continue; // no stable id — nothing to resolve against source_mapping

            std::string plex_type = item.value("type", "");
            BrowseContentItem entry;
            entry.external_id  = rating_key;
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
        } catch (...) { continue; }
    }
    return result;
}

std::optional<std::vector<BrowseContentItem>> PlexSource::fetchAllBrowseItems(const std::string& basePath) {
    constexpr int kPageSize = 200;
    std::vector<BrowseContentItem> result;
    int start = 0;
    bool first = true;
    while (true) {
        const std::string sep = basePath.find('?') != std::string::npos ? "&" : "?";
        const std::string path = basePath + sep +
            "X-Plex-Container-Start=" + std::to_string(start) +
            "&X-Plex-Container-Size=" + std::to_string(kPageSize);
        auto res = get(path);
        if (!res || res->status != 200) {
            if (first) return std::nullopt;
            break;
        }
        first = false;
        auto page = parseBrowseItems(res->body);
        result.insert(result.end(), page.begin(), page.end());
        if (static_cast<int>(page.size()) < kPageSize) break;
        start += kPageSize;
    }
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
    return fetchAllBrowseItems("/playlists/" + id + "/items").value_or(std::vector<BrowseContentItem>{});
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
    return fetchAllBrowseItems("/library/metadata/" + id + "/children").value_or(std::vector<BrowseContentItem>{});
}

std::optional<std::vector<PlexListItem>> PlexSource::fetchListItems(
    const std::string& id, const std::string& plex_type) {
    const std::string basePath = (plex_type == "playlist")
        ? "/playlists/" + id + "/items"
        : "/library/metadata/" + id + "/children";
    auto raw = fetchAllBrowseItems(basePath);
    if (!raw) return std::nullopt;
    std::vector<PlexListItem> result;
    result.reserve(raw->size());
    for (auto& item : *raw)
        result.push_back({item.item_type, item.external_id});
    return result;
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

// Shared parser for both plex.tv user-listing endpoints — they return the
// same MediaContainer.User[] shape with id/title/username/email fields.
std::vector<SourceUserInfo> parsePlexTvUsers(const std::string& body) {
    std::vector<SourceUserInfo> result;
    try {
        auto j = json::parse(body);
        if (!j.contains("users")) return result;
        for (const auto& u : j["users"]) {
        	
            SourceUserInfo info;
            info.external_user_id = std::to_string(u.value("id", 0));
            if (info.external_user_id.empty())
            {
            	DLOG << "[Plex skipping user: " << u.value("title", u.value("username", "")) << " No Valid ID]" << "\n";
	            continue;
            }
            info.display_name = u.value("title", "");
        	if (u.contains("email") && !u["email"].is_null())
            	info.email         = u["email"].get<std::string>();

        	result.push_back(std::move(info));
        }
    } catch (const json::exception& e) {
    	std::cerr << "[plex:] failed to query users: " << e.what() << '\n';
    }
    return result;
}

std::vector<SourceUserInfo> PlexSource::listServerUsers() {
    // plex.tv is a different host than base_url_ (the local server), but the
    // same account token authenticates both — no new credentials needed.
    httplib::Client account_client("https://plex.tv");
    account_client.set_default_headers({
        {"X-Plex-Token", token_},
        {"Accept",       "application/json"}
    });
    account_client.set_connection_timeout(10);
    account_client.set_read_timeout(15);

    std::vector<SourceUserInfo> merged;
    auto merge = [&](std::vector<SourceUserInfo> batch) {
        for (auto& u : batch) {
            auto it = std::find_if(merged.begin(), merged.end(),
                [&](const SourceUserInfo& e) { return e.external_user_id == u.external_user_id; });
            if (it == merged.end()) {
                merged.push_back(std::move(u));
            } else if (it->email.empty() && !u.email.empty()) {
                it->email = std::move(u.email);
            }
        }
    };
    std::vector<std::string> errors;
	
	// Get Device ID so we can grab users
	if (auto res = account_client.Get("/devices/"); res && res->status == 200)
	{
		auto device_list = json::parse(res->body);
		auto device = device_list[0];
		const int device_id = device["id"].get<int>();
		account_client.set_default_headers({
			{"X-Plex-Token", token_},
			{"X-Plex-Client-Identifier", std::to_string(device_id)},
			{"Accept",       "application/json"}
		});
	}
	else if (!res) {
		auto msg = "/devices/ — " + httplib::to_string(res.error());
		std::cerr << "[plex:" << source_id_ << "] " << msg << '\n';
		errors.push_back(msg);
	} else {
		auto msg = "/devices — HTTP " + std::to_string(res->status);
		std::cerr << "[plex:" << source_id_ << "] " << msg << '\n';
		errors.push_back(msg);
	}
	
	std::cerr << "[plex:" << source_id_ << " requesting users]" << '\n';

    // Home/managed users — PIN-only kid profiles typically have no email.
    if (auto res = account_client.Get("/api/v2/home/users"); res && res->status == 200)
        merge(parsePlexTvUsers(res->body));
    else if (!res) {
        auto msg = "/api/home/users — " + httplib::to_string(res.error());
        std::cerr << "[plex:" << source_id_ << "] " << msg << '\n';
        errors.push_back(msg);
    } else {
        auto msg = "/api/home/users — HTTP " + std::to_string(res->status);
        std::cerr << "[plex:" << source_id_ << "] " << msg << '\n';
        errors.push_back(msg);
    }

	// Friends account syncing doesnt work. hitting /api/users returns 200 but not with valid XML data.
	if (false) {
		// Shared ("Friends") access — separate Plex accounts, which do carry email.
    	if (auto res = account_client.Get("/api/users"); res && res->status == 200)
    		merge(parsePlexTvUsers(res->body));
    	else if (!res) {
    		auto msg = "/api/users — " + httplib::to_string(res.error());
    		std::cerr << "[plex:" << source_id_ << "] " << msg << '\n';
    		errors.push_back(msg);
    	} else {
    		auto msg = "/api/users — HTTP " + std::to_string(res->status);
    		std::cerr << "[plex:" << source_id_ << "] " << msg << '\n';
    		errors.push_back(msg);
    	}
	}

    // Both endpoints are independent (Home users vs Friends/shared access) —
    // only surface this as a diagnostic if it left us with nothing at all;
    // one failing while the other succeeds is a normal, harmless partial result.
    last_user_discovery_error_ = merged.empty() && !errors.empty()
        ? errors.front() + (errors.size() > 1 ? "; " + errors[1] : "")
        : "";

    return merged;
}

// ---------------------------------------------------------------------------
// Per-account watch state (for accounts other than the configured primary —
// see source_user.imported_user_id / SyncManager::syncLinkedUserWatchState)
// ---------------------------------------------------------------------------

std::vector<ExternalWatchState> PlexSource::fetchWatchState(const std::string& external_user_id) {
    // Step 1: exchange the admin token for one scoped to this specific
    // account. Only works for Plex Home/managed users — a "Friends" account
    // (a separate, full Plex login shared onto this server) has its own
    // credentials Kairos never has and never asks for, so this switch simply
    // fails for those and the account yields no data, same net effect as
    // "unsupported" without needing to track which kind of account this is.
    // A PIN-protected Home profile fails the same way, for the same reason
    // (Kairos doesn't collect/store PINs either).
    httplib::Client account_client(base_url_);
    account_client.set_default_headers({
        {"X-Plex-Token", token_},
        {"Accept",       "application/json"}
    });
    account_client.set_connection_timeout(10);
    account_client.set_read_timeout(15);

	DLOG << "[Plex requesting watch state for user: " << external_user_id << "]" << "\n";
    auto switch_res = account_client.Get("/status/sessions/history/all?accountID=" + external_user_id);
    if (!switch_res) {
        std::cerr << "[plex:" << source_id_ << "] User History error: " << httplib::to_string(switch_res.error()) << '\n';
        return {};
    }
    if (switch_res->status != 200) {
        std::cerr << "[plex:" << source_id_ << "] User History error: " << switch_res->status
                  << " (not a Home user, or needs a PIN Kairos doesn't have)\n";
        return {};
    }

	// /status/sessions/history/all is a per-playback-session log — one row per
	// watch, not a per-item total — so entries for a repeat-watched item must
	// be aggregated by ratingKey here rather than passed through 1:1, or
	// applyWatchState's view-count merge would see view_count=1 on every
	// duplicate and never accumulate past 1.
	std::unordered_map<std::string, ExternalWatchState> by_id;
    try {
        auto j = json::parse(switch_res->body);
        for (const auto& item : j["MediaContainer"].value("Metadata", json::array())) {
            std::string external_id = item.value("ratingKey", "");
            if (external_id.empty()) continue;

            auto& st = by_id[external_id];
            st.external_id = external_id;
            st.item_type   = (item.value("type", "") == "movie") ? "movie" : "episode";
        	st.title       = item.value("title", "");

        	// We're specifically querying watch history and plex doesn't give us current progress
        	// If we've made it here it's been watched.
            st.watched    = true;
            st.view_count = st.view_count.value_or(0) + 1;
            if (item.contains("viewOffset") && !item["viewOffset"].is_null())
                st.position_ms = item["viewOffset"].get<int64_t>();
            if (item.contains("viewedAt") && !item["viewedAt"].is_null()) {
                int64_t v = item["viewedAt"].get<int64_t>();
                if (v > st.watched_at.value_or(0)) st.watched_at = v;
            }
        }
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] parse error (watch state, user: "
                  << external_user_id << "): " << e.what() << '\n';
    }

    std::vector<ExternalWatchState> result;
    result.reserve(by_id.size());
    for (auto& [id, st] : by_id) result.push_back(std::move(st));
    return result;
}

namespace {
// std::to_string(double) always pads to 6 decimals ("8.100000") — valid
// for Plex to parse, but needless noise in the request/logs. Trims trailing
// zeros (and a trailing '.' if the value was a whole number).
std::string formatRating(double v) {
    std::string s = std::to_string(v);
    s.erase(s.find_last_not_of('0') + 1, std::string::npos);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string plexUrlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", c); out += buf; }
    }
    return out;
}

// item["Genre"]/["Director"]/etc. is an array of {"tag": "..."} objects —
// same shape the read side already parses in fetchShows/fetchMovies.
std::vector<std::string> extractTagArray(const json& item, const char* key) {
    std::vector<std::string> out;
    if (item.contains(key))
        for (const auto& t : item[key])
            out.push_back(t.value("tag", ""));
    return out;
}

// Appends Plex's tag-array edit directives for one field to `path`,
// diffing `current` (freshly fetched) against `desired` (what Pantheon
// wants) — see PlexSource::pushMetadata's own comment for why a diff, not
// a naive resend, is required. No-ops if the sets already match (nothing
// to add or remove).
void appendTagEdit(std::string& path, const std::string& plex_field,
                    const std::vector<std::string>& current,
                    const std::vector<std::string>& desired) {
    std::vector<std::string> to_remove;
    for (const auto& c : current)
        if (std::find(desired.begin(), desired.end(), c) == desired.end())
            to_remove.push_back(c);
    const bool already_matches = to_remove.empty() && desired.size() == current.size()
        && std::all_of(desired.begin(), desired.end(), [&](const std::string& d) {
               return std::find(current.begin(), current.end(), d) != current.end();
           });
    if (already_matches) return;

    if (!to_remove.empty()) {
        std::string joined;
        for (size_t i = 0; i < to_remove.size(); ++i) {
            if (i) joined += ",";
            joined += plexUrlEncode(to_remove[i]);
        }
        path += "&" + plex_field + "%5B%5D.tag.tag-=" + joined;
    }
    for (size_t i = 0; i < desired.size(); ++i)
        path += "&" + plex_field + "%5B" + std::to_string(i) + "%5D.tag.tag=" + plexUrlEncode(desired[i]);
    path += "&" + plex_field + ".locked=1";
}

// Parses a Pantheon JSON-array-string field (Show/Movie.genres etc.) into
// plain strings, silently yielding an empty list on malformed input.
std::vector<std::string> parseJsonStringArray(const std::string& json_str) {
    std::vector<std::string> out;
    try {
        for (const auto& v : json::parse(json_str))
            if (v.is_string()) out.push_back(v.get<std::string>());
    } catch (const json::exception&) {}
    return out;
}
}

bool PlexSource::pushMetadata(const std::string& external_id,
                               const std::string& external_lib_id,
                               const std::string& item_type,
                               const WritebackFields& fields) {
    if (external_lib_id.empty()) {
        std::cerr << "[plex:" << source_id_ << "] pushMetadata: no section id for id="
                  << external_id << '\n';
        return false;
    }

    bool ok = true;

    std::string path = "/library/sections/" + external_lib_id + "/all?type="
        + (item_type == "movie" ? "1" : "2") + "&id=" + external_id;

    auto addField = [&](const std::string& plex_field, const std::string& value) {
        if (value.empty()) return;
        path += "&" + plex_field + ".value=" + plexUrlEncode(value)
              + "&" + plex_field + ".locked=1";
    };
    addField("title",                fields.title);
    addField("originalTitle",        fields.original_title);
    addField("summary",              fields.overview);
    addField("contentRating",        fields.content_rating);
    addField("studio",               fields.studio);
    addField("tagline",              fields.tagline);
    addField("originallyAvailableAt", fields.release_date);
    if (fields.audience_rating)
        path += "&audienceRating.value=" + formatRating(*fields.audience_rating) + "&audienceRating.locked=1";

    // Array-valued fields (genre/director/writer/country/collection/label)
    // use Plex's add/remove tag-directive convention, not a whole-object
    // replace like Jellyfin's — verified against python-plexapi's actual
    // production implementation (github.com/pkkid/python-plexapi,
    // plexapi/mixins/edit.py: EditTagsMixin/_tagHelper) rather than
    // guessed: `{tag}[i].tag.tag` sets/adds an entry at that index,
    // `{tag}[].tag.tag-` (comma-joined, URL-escaped) removes specific
    // entries — there is no single "replace the whole list" directive. To
    // get writeback's "set to exactly this list" semantics, the live
    // current tags are fetched fresh right here (a stale local copy could
    // miss something a user just changed in Plex itself) and diffed
    // against the desired list — appendTagEdit() sends both the removal
    // directive for anything dropped and the full desired list as indices
    // in the same request.
    //
    // Cast/actors is NOT sent, and isn't merely deferred — even
    // python-plexapi, the reference client library every other tool in
    // this space (Kometa included) is built on, has no Role/Actor edit
    // mixin at all. There is no known-safe write mechanism to verify
    // against.
    //
    // director/writer/country only apply to movies on Plex's side (its own
    // ShowEditMixins has no Director/Writer/Country mixin) — Pantheon's own
    // WritebackFields.director/writer are already movie-only by convention
    // (only ever populated from ContentService::writebackMovie), so those
    // two need no extra guard; countries is guarded explicitly below since
    // WritebackFields.countries is populated for shows too.
    const bool needs_current_tags =
        !fields.genres.empty() || !fields.director.empty() || !fields.writer.empty() ||
        !fields.countries.empty() || !fields.collections.empty() || !fields.labels.empty();
    if (needs_current_tags) {
        auto cur_res = get("/library/metadata/" + external_id);
        if (cur_res && cur_res->status == 200) {
            try {
                const json cur = json::parse(cur_res->body);
                const auto& metadata = cur.value("MediaContainer", json::object()).value("Metadata", json::array());
                if (!metadata.empty()) {
                    const json& item = metadata[0];
                    if (!fields.genres.empty())
                        appendTagEdit(path, "genre", extractTagArray(item, "Genre"), parseJsonStringArray(fields.genres));
                    if (!fields.director.empty())
                        appendTagEdit(path, "director", extractTagArray(item, "Director"), {fields.director});
                    if (!fields.writer.empty())
                        appendTagEdit(path, "writer", extractTagArray(item, "Writer"), {fields.writer});
                    if (item_type == "movie" && !fields.countries.empty())
                        appendTagEdit(path, "country", extractTagArray(item, "Country"), parseJsonStringArray(fields.countries));
                    if (!fields.collections.empty())
                        appendTagEdit(path, "collection", extractTagArray(item, "Collection"), parseJsonStringArray(fields.collections));
                    if (!fields.labels.empty())
                        appendTagEdit(path, "label", extractTagArray(item, "Label"), parseJsonStringArray(fields.labels));
                }
            } catch (const json::exception& e) {
                std::cerr << "[plex:" << source_id_ << "] pushMetadata: couldn't parse current tags (id="
                          << external_id << "): " << e.what() << " — tag fields skipped this push\n";
            }
        } else {
            std::cerr << "[plex:" << source_id_ << "] pushMetadata: couldn't fetch current item for tag diff (id="
                      << external_id << ") — tag fields skipped this push\n";
        }
    }

    auto res = client_.Put(path.c_str());
    if (!res) {
        std::cerr << "[plex:" << source_id_ << "] pushMetadata failed (id=" << external_id
                  << "): " << httplib::to_string(res.error()) << '\n';
        ok = false;
    } else if (res->status != 200) {
        std::cerr << "[plex:" << source_id_ << "] pushMetadata HTTP " << res->status
                  << " (id=" << external_id << ")\n";
        ok = false;
    }

    // Poster/background upload — POSTs raw image bytes to Plex's own
    // per-item art endpoints, the same mechanism as "upload custom poster"
    // in Plex Web. Unlike the tag fields above, this is a well-established,
    // unambiguous convention rather than a guess, so it's not deferred.
    auto uploadArt = [&](const char* kind, const WritebackImage& img) {
        std::string art_path = "/library/metadata/" + external_id + "/" + kind;
        auto art_res = client_.Post(art_path.c_str(), img.bytes, img.content_type);
        if (!art_res || (art_res->status != 200 && art_res->status != 201)) {
            std::cerr << "[plex:" << source_id_ << "] pushMetadata (" << kind
                      << ") failed (id=" << external_id << ")\n";
            ok = false;
        }
    };
    if (fields.thumb) uploadArt("posters", *fields.thumb);
    if (fields.art)   uploadArt("arts",    *fields.art);

    return ok;
}

// ---------------------------------------------------------------------------
// List push (write)
// ---------------------------------------------------------------------------

std::string PlexSource::machineIdentifier() {
    if (machine_identifier_) return *machine_identifier_;
    std::string id;
    auto res = get("/identity");
    if (res && res->status == 200) {
        try {
            auto j = json::parse(res->body);
            id = j["MediaContainer"].value("machineIdentifier", "");
        } catch (const json::exception&) {}
    }
    if (id.empty())
        std::cerr << "[plex:" << source_id_ << "] failed to fetch machineIdentifier — list push unavailable\n";
    machine_identifier_ = id;
    return id;
}

namespace {
// server://{machineId}/com.plexapp.plugins.library/library/metadata/{ratingKey1,ratingKey2,...}
// — the URI scheme Plex's own playlist/collection create+add endpoints take
// to reference existing library items (confirmed against python-plexapi's
// actual request construction, not just prose docs).
std::string plexItemsUri(const std::string& machine_id, const std::vector<IMediaSource::PushListItem>& items) {
    std::string keys;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) keys += ",";
        keys += items[i].external_id;
    }
    return "server://" + machine_id + "/com.plexapp.plugins.library/library/metadata/" + keys;
}
} // namespace

std::optional<std::string> PlexSource::createRemoteList(
    const std::string& title, const std::string& kind,
    const std::vector<IMediaSource::PushListItem>& items,
    const std::string& external_lib_id) {
    if (items.empty()) return std::nullopt;
    const std::string machine_id = machineIdentifier();
    if (machine_id.empty()) return std::nullopt;

    const std::string uri = plexUrlEncode(plexItemsUri(machine_id, items));
    // Collections are scoped to a library section (sectionId); playlists are
    // server-wide, no section needed — see IMediaSource::createRemoteList's
    // doc comment on external_lib_id.
    const std::string path = (kind == "collection")
        ? "/library/collections?uri=" + uri + "&type=video&title=" + plexUrlEncode(title)
              + "&smart=0&sectionId=" + plexUrlEncode(external_lib_id)
        : "/playlists?uri=" + uri + "&type=video&title=" + plexUrlEncode(title) + "&smart=0";

    auto res = client_.Post(path.c_str());
    if (!res || (res->status != 200 && res->status != 201)) {
        std::cerr << "[plex:" << source_id_ << "] createRemoteList failed (" << kind << " \"" << title << "\"): "
                  << (res ? "HTTP " + std::to_string(res->status) : httplib::to_string(res.error())) << '\n';
        return std::nullopt;
    }
    try {
        auto j = json::parse(res->body);
        const auto& md = j["MediaContainer"]["Metadata"];
        if (!md.empty()) return md[0].value("ratingKey", "");
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] createRemoteList: couldn't parse response: " << e.what() << '\n';
    }
    return std::nullopt;
}

bool PlexSource::addRemoteListItems(const std::string& list_external_id, const std::string& kind,
                                     const std::vector<IMediaSource::PushListItem>& items) {
    if (items.empty()) return true;
    const std::string machine_id = machineIdentifier();
    if (machine_id.empty()) return false;

    const std::string uri  = plexUrlEncode(plexItemsUri(machine_id, items));
    const std::string base = (kind == "collection") ? "/library/collections/" + list_external_id
                                                     : "/playlists/" + list_external_id;
    auto res = client_.Put((base + "/items?uri=" + uri).c_str());
    if (!res || (res->status != 200 && res->status != 201)) {
        std::cerr << "[plex:" << source_id_ << "] addRemoteListItems failed (" << kind << " " << list_external_id << ")\n";
        return false;
    }
    return true;
}

bool PlexSource::removeRemoteListItems(const std::string& list_external_id, const std::string& kind,
                                        const std::vector<IMediaSource::PushListItem>& items) {
    if (items.empty()) return true;

    // Collections remove by the item's own ratingKey directly — unlike
    // playlists, items can't repeat in a collection, so there's no separate
    // per-entry id to resolve first.
    if (kind == "collection") {
        bool ok = true;
        for (const auto& item : items) {
            auto res = client_.Delete(("/library/collections/" + list_external_id + "/items/" + item.external_id).c_str());
            if (!res || (res->status != 200 && res->status != 204)) {
                std::cerr << "[plex:" << source_id_ << "] removeRemoteListItems: failed to remove " << item.external_id
                          << " from collection " << list_external_id << '\n';
                ok = false;
            }
        }
        return ok;
    }

    // Playlists have no remove-by-ratingKey form — each entry's distinct
    // playlistItemID (separate from the underlying media's ratingKey) is
    // only obtainable by fetching the playlist's current items first.
    auto res = get("/playlists/" + list_external_id + "/items");
    if (!res || res->status != 200) {
        std::cerr << "[plex:" << source_id_ << "] removeRemoteListItems: couldn't fetch playlist items for "
                  << list_external_id << '\n';
        return false;
    }
    std::unordered_map<std::string, std::string> entryIdByRatingKey;
    try {
        auto j = json::parse(res->body);
        const auto& md = j["MediaContainer"];
        if (md.contains("Metadata")) {
            for (const auto& entry : md["Metadata"]) {
                std::string rk       = entry.value("ratingKey", "");
                std::string entry_id = entry.value("playlistItemID", "");
                if (!rk.empty() && !entry_id.empty()) entryIdByRatingKey[rk] = entry_id;
            }
        }
    } catch (const json::exception& e) {
        std::cerr << "[plex:" << source_id_ << "] removeRemoteListItems: parse error: " << e.what() << '\n';
        return false;
    }

    bool ok = true;
    for (const auto& item : items) {
        auto it = entryIdByRatingKey.find(item.external_id);
        if (it == entryIdByRatingKey.end()) { ok = false; continue; }
        auto del = client_.Delete(("/playlists/" + list_external_id + "/items/" + it->second).c_str());
        if (!del || (del->status != 200 && del->status != 204)) {
            std::cerr << "[plex:" << source_id_ << "] removeRemoteListItems: failed to remove entry " << it->second
                      << " from playlist " << list_external_id << '\n';
            ok = false;
        }
    }
    return ok;
}
