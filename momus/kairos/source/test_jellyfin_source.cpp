// Tests for JellyfinBaseSource parsing logic via a local httplib stub server.
// Covers JellyfinSource and EmbySource: all fetch/browse methods, pagination,
// field mapping, ticks→ms conversion, error paths, and auth header differences.
//
// fetchEpisodes builds its own httplib::Client internally (thread-safety for
// concurrent sync), so a real local server is required — overriding get() alone
// would not cover it.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "source/JellyfinSource.h"
#include "source/EmbySource.h"
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using json = nlohmann::json;

// RunTimeTicks are 100-nanosecond units; divide by 10'000 to get milliseconds.
static constexpr int64_t kTwoHourTicks = 72'000'000'000LL;   // 7 200 000 ms
static constexpr int64_t kOneHourTicks = 36'000'000'000LL;   // 3 600 000 ms
static constexpr int64_t k45MinTicks   = 27'000'000'000LL;   // 2 700 000 ms
static constexpr int64_t k48MinTicks   = 28'800'000'000LL;   // 2 880 000 ms
static constexpr int64_t k43MinTicks   = 26'100'000'000LL;   // 2 610 000 ms

// ============================================================================
// TestServer — binds to a random port; register routes before calling start()
// ============================================================================

struct TestServer {
    httplib::Server svr;
    int             port{0};
    std::thread     thread;

    TestServer() { port = svr.bind_to_any_port("127.0.0.1"); }

    void start() {
        thread = std::thread([this] { svr.listen_after_bind(); });
        while (!svr.is_running())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ~TestServer() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

// ============================================================================
// Canned route registration — called once on the shared fixture server
// ============================================================================

static void registerRoutes(httplib::Server& s) {

    // ── Library views ──────────────────────────────────────────────────────
    s.Get("/Users/uid/Views", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"Items", {
            {{"Id","lib-tv"},     {"Name","TV Shows"},    {"CollectionType","tvshows"}},
            {{"Id","lib-movies"}, {"Name","Movies"},      {"CollectionType","movies"}},
            {{"Id","lib-music"},  {"Name","Music"},       {"CollectionType","music"}},
            {{"Id","lib-mixed"},  {"Name","Home Videos"}, {"CollectionType","homevideos"}},
        }}}.dump(), "application/json");
    });

    // ── Shows / movies / browse — dispatched on query params ───────────────
    s.Get("/Users/uid/Items", [](const httplib::Request& req, httplib::Response& res) {
        const std::string type   = req.get_param_value("IncludeItemTypes");
        const std::string parent = req.get_param_value("ParentId");
        const int         start  = [&] {
            const auto v = req.get_param_value("StartIndex");
            return v.empty() ? 0 : std::stoi(v);
        }();

        // Paginated show library: page 0 → 500 rows, page 1 → 3 rows.
        if (type == "Series" && parent == "lib-tv-paged") {
            json items = json::array();
            const int n = (start == 0) ? 500 : 3;
            for (int i = 0; i < n; ++i) {
                const std::string sid = "ps" + std::to_string(start + i);
                items.push_back({{"Id", sid}, {"Name", "Show " + sid}});
            }
            res.set_content(json{{"Items", items}}.dump(), "application/json");
            return;
        }

        // Paginated movie library: page 0 → 500 rows, page 1 → 2 rows.
        if (type == "Movie" && parent == "lib-movies-paged") {
            json items = json::array();
            const int n = (start == 0) ? 500 : 2;
            for (int i = 0; i < n; ++i) {
                const std::string mid = "pm" + std::to_string(start + i);
                items.push_back({{"Id", mid}, {"Name", "Movie " + mid},
                    {"MediaSources", {{{"Path", "/media/" + mid + ".mkv"}}}}});
            }
            res.set_content(json{{"Items", items}}.dump(), "application/json");
            return;
        }

        if (type == "Series") {
            res.set_content(json{{"Items", {
                {{"Id","show1"}, {"Name","Test Show"},
                 {"OfficialRating","TV-MA"}, {"Overview","A gripping drama."},
                 {"Status","Continuing"}, {"ProductionYear",2020},
                 {"CommunityRating",8.5},
                 {"PremiereDate","2020-03-15T12:30:00Z"},
                 {"Studios",{{{"Name","Netflix"}}}},
                 {"Genres",{"Drama","Thriller"}},
                 {"Tags",{"crime"}},
                 {"ProductionLocations",{"US","UK"}},
                 {"People",{
                     {{"Type","Actor"},{"Name","Alice"}},
                     {{"Type","Actor"},{"Name","Bob"}},
                     {{"Type","Director"},{"Name","Dir Smith"}}
                 }},
                 {"ProviderIds",{{"Imdb","tt1234567"},{"Tvdb","123456"},{"Tmdb","789"}}},
                 {"ImageTags",{{"Primary","abc123"}}},
                 {"BackdropImageTags",{"back1"}}}
            }}}.dump(), "application/json");
            return;
        }

        if (type == "Movie") {
            const int64_t twoHr = kTwoHourTicks;
            const int64_t oneHr = kOneHourTicks;
            res.set_content(json{{"Items", {
                {   // Full fields; top-level RunTimeTicks present.
                    {"Id","movie1"}, {"Name","Test Movie"},
                    {"OfficialRating","R"}, {"Overview","A test movie."},
                    {"Tagline","See it or else."}, {"ProductionYear",2021},
                    {"CommunityRating",7.2},
                    {"RunTimeTicks",twoHr},
                    {"Studios",{{{"Name","WB"}}}},
                    {"Genres",{"Action","Sci-Fi"}},
                    {"Tags",{"blockbuster"}},
                    {"ProductionLocations",{"US"}},
                    {"People",{
                        {{"Type","Director"},{"Name","Dir One"}},
                        {{"Type","Actor"},{"Name","Star One"}}
                    }},
                    {"ProviderIds",{{"Imdb","tt7654321"},{"Tmdb","456"}}},
                    {"ImageTags",{{"Primary","mov-thumb"}}},
                    {"BackdropImageTags",{"mov-back"}},
                    {"MediaSources",{{{"Path","/media/movie1.mkv"},{"RunTimeTicks",twoHr}}}}
                },
                {   // No top-level RunTimeTicks → must fall back to MediaSources[0].
                    {"Id","movie2"}, {"Name","Fallback Duration"},
                    {"MediaSources",{{{"Path","/media/movie2.mkv"},{"RunTimeTicks",oneHr}}}}
                },
                {   // No MediaSources path → must be skipped.
                    {"Id","movie3"}, {"Name","No Path Movie"},
                    {"MediaSources",json::array()}
                }
            }}}.dump(), "application/json");
            return;
        }

        if (type == "Playlist") {
            res.set_content(json{{"Items", {
                {{"Id","pl1"},{"Name","Playlist One"},{"ChildCount",5}},
                {{"Id","pl2"},{"Name","Playlist Two"},{"ChildCount",12}}
            }}}.dump(), "application/json");
            return;
        }

        if (type == "BoxSet") {
            res.set_content(json{{"Items", {
                {{"Id","col1"},{"Name","Marvel Collection"},{"ChildCount",25}}
            }}}.dump(), "application/json");
            return;
        }

        // Collection items: IncludeItemTypes=Movie,Episode + ParentId=col1.
        if (parent == "col1") {
            const int64_t twoHr = kTwoHourTicks;
            const int64_t k45   = k45MinTicks;
            res.set_content(json{{"Items", {
                {{"Id","cm1"},{"Type","Movie"},{"Name","Avengers"},{"RunTimeTicks",twoHr}},
                {{"Id","ce1"},{"Type","Episode"},{"Name","Ep A"},{"RunTimeTicks",k45},
                 {"SeriesName","Show B"},{"ParentIndexNumber",2},{"IndexNumber",3}}
            }}}.dump(), "application/json");
            return;
        }

        res.status = 400;
    });

    // ── Seasons ────────────────────────────────────────────────────────────
    s.Get("/Shows/show1/Seasons", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"Items", {
            {{"IndexNumber",1},{"Name","Season One"}},
            {{"IndexNumber",2},{"Name","Season Two"}}
        }}}.dump(), "application/json");
    });

    // ── Episodes ───────────────────────────────────────────────────────────
    s.Get("/Shows/show1/Episodes", [](const httplib::Request&, httplib::Response& res) {
        const int64_t t45 = k45MinTicks;
        const int64_t t48 = k48MinTicks;
        const int64_t t43 = k43MinTicks;
        res.set_content(json{{"Items", {
            {   // ep1: full fields, S1E1
                {"Id","ep1"}, {"SeriesId","show1"}, {"Name","Pilot"},
                {"ParentIndexNumber",1}, {"IndexNumber",1},
                {"RunTimeTicks",t45},
                {"AbsoluteEpisodeNumber",1},
                {"Overview","First episode."},
                {"PremiereDate","2020-03-15T00:00:00Z"},
                {"ImageTags",{{"Primary","ep1thumb"}}},
                {"MediaSources",{{{"Path","/media/s01e01.mkv"}}}}
            },
            {   // ep2: S1E2, no AbsoluteEpisodeNumber, no Overview, no ImageTags
                {"Id","ep2"}, {"SeriesId","show1"}, {"Name","Episode 2"},
                {"ParentIndexNumber",1}, {"IndexNumber",2},
                {"RunTimeTicks",t48},
                {"MediaSources",{{{"Path","/media/s01e02.mkv"}}}}
            },
            {   // ep3: S2E1 — for sort-order verification
                {"Id","ep3"}, {"SeriesId","show1"}, {"Name","S2 Ep1"},
                {"ParentIndexNumber",2}, {"IndexNumber",1},
                {"RunTimeTicks",t43},
                {"MediaSources",{{{"Path","/media/s02e01.mkv"}}}}
            },
            {   // ep_skip: no MediaSources path → must be excluded
                {"Id","ep_skip"}, {"SeriesId","show1"}, {"Name","No Path"},
                {"ParentIndexNumber",1}, {"IndexNumber",99},
                {"MediaSources",json::array()}
            }
        }}}.dump(), "application/json");
    });

    // ── Playlist items ─────────────────────────────────────────────────────
    s.Get("/Playlists/pl1/Items", [](const httplib::Request&, httplib::Response& res) {
        const int64_t twoHr = kTwoHourTicks;
        const int64_t k45   = k45MinTicks;
        res.set_content(json{{"Items", {
            {{"Id","pm1"},{"Type","Movie"},{"Name","Movie A"},{"RunTimeTicks",twoHr}},
            {{"Id","pe1"},{"Type","Episode"},{"Name","Ep X"},{"RunTimeTicks",k45},
             {"SeriesName","Show Z"},{"ParentIndexNumber",1},{"IndexNumber",3}}
        }}}.dump(), "application/json");
    });

    // ── Error paths ────────────────────────────────────────────────────────
    s.Get("/Users/err-uid/Views",     [](const httplib::Request&, httplib::Response& r){ r.status=500; });
    s.Get("/Users/err-uid/Items",     [](const httplib::Request&, httplib::Response& r){ r.status=500; });
    s.Get("/Shows/bad-show/Seasons",  [](const httplib::Request&, httplib::Response& r){ r.status=500; });
    s.Get("/Shows/bad-show/Episodes", [](const httplib::Request&, httplib::Response& r){ r.status=500; });
}

// ============================================================================
// Fixture — shared server (started once), fresh JellyfinSource per test
// ============================================================================

class JellyfinSourceTest : public ::testing::Test {
public:
    static void SetUpTestSuite() {
        srv_ = new TestServer();
        registerRoutes(srv_->svr);
        srv_->start();
    }
    static void TearDownTestSuite() {
        delete srv_;
        srv_ = nullptr;
    }

protected:
    void SetUp() override {
        src_ = std::make_unique<JellyfinSource>("src1", srv_->url(), "tok", "uid");
    }

    static TestServer*              srv_;
    std::unique_ptr<JellyfinSource> src_;
};

TestServer* JellyfinSourceTest::srv_ = nullptr;

// ============================================================================
// listAvailableLibraries
// ============================================================================

TEST_F(JellyfinSourceTest, ListLibraries_Count) {
    EXPECT_EQ(src_->listAvailableLibraries().size(), 4u);
}

TEST_F(JellyfinSourceTest, ListLibraries_CollectionTypeMapping) {
    const auto libs = src_->listAvailableLibraries();
    EXPECT_EQ(libs[0].type, "show");
    EXPECT_EQ(libs[1].type, "movie");
    EXPECT_EQ(libs[2].type, "music");
    EXPECT_EQ(libs[3].type, "mixed");
}

TEST_F(JellyfinSourceTest, ListLibraries_Fields) {
    const auto libs = src_->listAvailableLibraries();
    EXPECT_EQ(libs[0].external_lib_id, "lib-tv");
    EXPECT_EQ(libs[0].name,            "TV Shows");
}

TEST_F(JellyfinSourceTest, ListLibraries_EmptyOnServerError) {
    JellyfinSource err("e", srv_->url(), "tok", "err-uid");
    EXPECT_TRUE(err.listAvailableLibraries().empty());
}

// ============================================================================
// fetchShows
// ============================================================================

TEST_F(JellyfinSourceTest, FetchShows_CoreFields) {
    const auto shows = src_->fetchShows("lib-tv");
    ASSERT_EQ(shows.size(), 1u);
    const auto& s = shows[0];
    EXPECT_EQ(s.show_id,        "show1");
    EXPECT_EQ(s.title,          "Test Show");
    EXPECT_EQ(s.content_rating, "TV-MA");
    EXPECT_EQ(s.overview,       "A gripping drama.");
    EXPECT_EQ(s.status,         "Continuing");
}

TEST_F(JellyfinSourceTest, FetchShows_YearAndRating) {
    const auto shows = src_->fetchShows("lib-tv");
    ASSERT_FALSE(shows.empty());
    const auto& s = shows[0];
    ASSERT_TRUE(s.year.has_value());
    EXPECT_EQ(*s.year, 2020);
    ASSERT_TRUE(s.audience_rating.has_value());
    EXPECT_NEAR(*s.audience_rating, 8.5f, 0.01f);
}

TEST_F(JellyfinSourceTest, FetchShows_ProviderIds) {
    const auto shows = src_->fetchShows("lib-tv");
    ASSERT_FALSE(shows.empty());
    const auto& s = shows[0];
    EXPECT_EQ(s.imdb_id, "tt1234567");
    EXPECT_EQ(s.tvdb_id, "123456");
    EXPECT_EQ(s.tmdb_id, "789");
}

TEST_F(JellyfinSourceTest, FetchShows_StudioAndNetworkFromStudiosArray) {
    const auto shows = src_->fetchShows("lib-tv");
    ASSERT_FALSE(shows.empty());
    const auto& s = shows[0];
    EXPECT_EQ(s.studio,  "Netflix");
    EXPECT_EQ(s.network, "Netflix");
}

TEST_F(JellyfinSourceTest, FetchShows_ArrayFieldsAsJsonStrings) {
    const auto shows = src_->fetchShows("lib-tv");
    ASSERT_FALSE(shows.empty());
    const auto& s = shows[0];
    EXPECT_EQ(s.genres,    "[\"Drama\",\"Thriller\"]");
    EXPECT_EQ(s.labels,    "[\"crime\"]");
    EXPECT_EQ(s.countries, "[\"US\",\"UK\"]");
    EXPECT_EQ(s.actors,    "[\"Alice\",\"Bob\"]"); // Director excluded
}

TEST_F(JellyfinSourceTest, FetchShows_ImagePaths) {
    const auto shows = src_->fetchShows("lib-tv");
    ASSERT_FALSE(shows.empty());
    const auto& s = shows[0];
    EXPECT_EQ(s.thumb, "/Items/show1/Images/Primary");
    EXPECT_EQ(s.art,   "/Items/show1/Images/Backdrop");
}

TEST_F(JellyfinSourceTest, FetchShows_PremiereDate_TruncatedToDate) {
    EXPECT_EQ(src_->fetchShows("lib-tv")[0].originally_available_at, "2020-03-15");
}

TEST_F(JellyfinSourceTest, FetchShows_Pagination) {
    // Page 0 returns 500, page 1 returns 3 → total 503.
    EXPECT_EQ(src_->fetchShows("lib-tv-paged").size(), 503u);
}

TEST_F(JellyfinSourceTest, FetchShows_EmptyOnServerError) {
    JellyfinSource err("e", srv_->url(), "tok", "err-uid");
    EXPECT_TRUE(err.fetchShows("lib-tv").empty());
}

// ============================================================================
// fetchMovies
// ============================================================================

TEST_F(JellyfinSourceTest, FetchMovies_SkipsItemWithNoPath) {
    // movie3 has no MediaSources path → only 2 results.
    EXPECT_EQ(src_->fetchMovies("lib-movies").size(), 2u);
}

TEST_F(JellyfinSourceTest, FetchMovies_CoreFields) {
    const auto movies = src_->fetchMovies("lib-movies");
    ASSERT_FALSE(movies.empty());
    const auto& m = movies[0];
    EXPECT_EQ(m.movie_id,       "movie1");
    EXPECT_EQ(m.title,          "Test Movie");
    EXPECT_EQ(m.content_rating, "R");
    EXPECT_EQ(m.overview,       "A test movie.");
    EXPECT_EQ(m.tagline,        "See it or else.");
    EXPECT_EQ(m.file_path,      "/media/movie1.mkv");
    ASSERT_TRUE(m.year.has_value());
    EXPECT_EQ(*m.year, 2021);
}

TEST_F(JellyfinSourceTest, FetchMovies_DurationFromTopLevelTicks) {
    const auto movies = src_->fetchMovies("lib-movies");
    ASSERT_FALSE(movies.empty());
    EXPECT_EQ(movies[0].duration_ms, kTwoHourTicks / 10000);
}

TEST_F(JellyfinSourceTest, FetchMovies_DurationFallsBackToMediaSources) {
    // movie2 has no top-level RunTimeTicks — must use MediaSources[0].RunTimeTicks.
    const auto movies = src_->fetchMovies("lib-movies");
    ASSERT_EQ(movies.size(), 2u);
    EXPECT_EQ(movies[1].movie_id,    "movie2");
    EXPECT_EQ(movies[1].duration_ms, kOneHourTicks / 10000);
}

TEST_F(JellyfinSourceTest, FetchMovies_ProviderIds) {
    const auto movies = src_->fetchMovies("lib-movies");
    ASSERT_FALSE(movies.empty());
    EXPECT_EQ(movies[0].imdb_id, "tt7654321");
    EXPECT_EQ(movies[0].tmdb_id, "456");
}

TEST_F(JellyfinSourceTest, FetchMovies_DirectorAndActors) {
    const auto movies = src_->fetchMovies("lib-movies");
    ASSERT_FALSE(movies.empty());
    EXPECT_EQ(movies[0].director, "Dir One");
    EXPECT_EQ(movies[0].actors,   "[\"Star One\"]");
}

TEST_F(JellyfinSourceTest, FetchMovies_ImagePaths) {
    const auto movies = src_->fetchMovies("lib-movies");
    ASSERT_FALSE(movies.empty());
    EXPECT_EQ(movies[0].thumb, "/Items/movie1/Images/Primary");
    EXPECT_EQ(movies[0].art,   "/Items/movie1/Images/Backdrop");
}

TEST_F(JellyfinSourceTest, FetchMovies_Pagination) {
    // Page 0 returns 500, page 1 returns 2 → total 502.
    EXPECT_EQ(src_->fetchMovies("lib-movies-paged").size(), 502u);
}

TEST_F(JellyfinSourceTest, FetchMovies_EmptyOnServerError) {
    JellyfinSource err("e", srv_->url(), "tok", "err-uid");
    EXPECT_TRUE(err.fetchMovies("lib-movies").empty());
}

// ============================================================================
// fetchEpisodes
// ============================================================================

TEST_F(JellyfinSourceTest, FetchEpisodes_SkipsItemWithNoPath) {
    // 4 items in server response; ep_skip has no MediaSources path → 3 valid.
    EXPECT_EQ(src_->fetchEpisodes("show1").size(), 3u);
}

TEST_F(JellyfinSourceTest, FetchEpisodes_CoreFields) {
    const auto eps = src_->fetchEpisodes("show1");
    ASSERT_FALSE(eps.empty());
    const auto& e = eps[0]; // ep1: S1E1 after sort
    EXPECT_EQ(e.episode_id, "ep1");
    EXPECT_EQ(e.show_id,    "show1");
    EXPECT_EQ(e.title,      "Pilot");
    EXPECT_EQ(e.file_path,  "/media/s01e01.mkv");
    EXPECT_EQ(e.overview,   "First episode.");
    EXPECT_EQ(e.season,     1);
    EXPECT_EQ(e.episode,    1);
}

TEST_F(JellyfinSourceTest, FetchEpisodes_Duration) {
    const auto eps = src_->fetchEpisodes("show1");
    EXPECT_EQ(eps[0].duration_ms, k45MinTicks / 10000);
    EXPECT_EQ(eps[1].duration_ms, k48MinTicks / 10000);
    EXPECT_EQ(eps[2].duration_ms, k43MinTicks / 10000);
}

TEST_F(JellyfinSourceTest, FetchEpisodes_SeasonNameFromSeasonsList) {
    const auto eps = src_->fetchEpisodes("show1");
    EXPECT_EQ(eps[0].season_name, "Season One"); // ep1 S1
    EXPECT_EQ(eps[1].season_name, "Season One"); // ep2 S1
    EXPECT_EQ(eps[2].season_name, "Season Two"); // ep3 S2
}

TEST_F(JellyfinSourceTest, FetchEpisodes_AbsoluteIndex) {
    const auto eps = src_->fetchEpisodes("show1");
    ASSERT_TRUE(eps[0].absolute_index.has_value());
    EXPECT_EQ(*eps[0].absolute_index, 1);
    EXPECT_FALSE(eps[1].absolute_index.has_value()); // ep2 has none
}

TEST_F(JellyfinSourceTest, FetchEpisodes_SortedBySeasonThenEpisode) {
    const auto eps = src_->fetchEpisodes("show1");
    ASSERT_EQ(eps.size(), 3u);
    EXPECT_EQ(eps[0].episode_id, "ep1"); // S1E1
    EXPECT_EQ(eps[1].episode_id, "ep2"); // S1E2
    EXPECT_EQ(eps[2].episode_id, "ep3"); // S2E1
}

TEST_F(JellyfinSourceTest, FetchEpisodes_AirDateTruncatedToDate) {
    const auto eps = src_->fetchEpisodes("show1");
    EXPECT_EQ(eps[0].air_date, "2020-03-15"); // ep1 has PremiereDate
    EXPECT_EQ(eps[1].air_date, "");           // ep2 has none
}

TEST_F(JellyfinSourceTest, FetchEpisodes_ThumbPath) {
    const auto eps = src_->fetchEpisodes("show1");
    EXPECT_EQ(eps[0].thumb, "/Items/ep1/Images/Primary"); // ep1 has ImageTags
    EXPECT_EQ(eps[1].thumb, "");                          // ep2 has none
}

TEST_F(JellyfinSourceTest, FetchEpisodes_EmptyOnServerError) {
    EXPECT_TRUE(src_->fetchEpisodes("bad-show").empty());
}

// ============================================================================
// fetchPlaylists — intentionally empty (browse API covers the live-query case)
// ============================================================================

TEST_F(JellyfinSourceTest, FetchPlaylists_AlwaysEmpty) {
    EXPECT_TRUE(src_->fetchPlaylists("lib-tv").empty());
}

// ============================================================================
// Browse
// ============================================================================

TEST_F(JellyfinSourceTest, BrowsePlaylists_CountAndFields) {
    const auto lists = src_->browsePlaylists();
    ASSERT_EQ(lists.size(), 2u);
    EXPECT_EQ(lists[0].id,         "pl1");
    EXPECT_EQ(lists[0].title,      "Playlist One");
    EXPECT_EQ(lists[0].item_count, 5);
    EXPECT_EQ(lists[1].item_count, 12);
}

TEST_F(JellyfinSourceTest, BrowsePlaylistItems_TypeDispatchAndDuration) {
    const auto items = src_->browsePlaylistItems("pl1");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].item_type,   "movie");
    EXPECT_EQ(items[0].duration_ms, kTwoHourTicks / 10000);
    EXPECT_EQ(items[1].item_type,   "episode");
    EXPECT_EQ(items[1].show_title,  "Show Z");
    EXPECT_EQ(items[1].season,      1);
    EXPECT_EQ(items[1].episode,     3);
    EXPECT_EQ(items[1].duration_ms, k45MinTicks / 10000);
}

TEST_F(JellyfinSourceTest, BrowseCollections_CountAndFields) {
    const auto cols = src_->browseCollections("");
    ASSERT_EQ(cols.size(), 1u);
    EXPECT_EQ(cols[0].id,         "col1");
    EXPECT_EQ(cols[0].title,      "Marvel Collection");
    EXPECT_EQ(cols[0].item_count, 25);
}

TEST_F(JellyfinSourceTest, BrowseCollectionItems_TypeDispatchAndDuration) {
    const auto items = src_->browseCollectionItems("col1");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].item_type,   "movie");
    EXPECT_EQ(items[0].duration_ms, kTwoHourTicks / 10000);
    EXPECT_EQ(items[1].item_type,   "episode");
    EXPECT_EQ(items[1].show_title,  "Show B");
    EXPECT_EQ(items[1].season,      2);
    EXPECT_EQ(items[1].episode,     3);
}

// ============================================================================
// Source identity and auth headers
// ============================================================================

TEST(JellyfinMeta, SourceTypeIsJellyfin) {
    JellyfinSource src("s1", "http://127.0.0.1:1", "tok", "uid");
    EXPECT_EQ(src.sourceType(), "jellyfin");
}

TEST(JellyfinMeta, IsSupported) {
    JellyfinSource src("s1", "http://127.0.0.1:1", "tok", "uid");
    EXPECT_TRUE(src.isSupported());
}

TEST(EmbyMeta, SourceTypeIsEmby) {
    EmbySource src("s1", "http://127.0.0.1:1", "tok", "uid");
    EXPECT_EQ(src.sourceType(), "emby");
}

TEST(EmbyMeta, IsSupported) {
    EmbySource src("s1", "http://127.0.0.1:1", "tok", "uid");
    EXPECT_TRUE(src.isSupported());
}

TEST(JellyfinMeta, SendsXMediaBrowserTokenHeader) {
    TestServer srv;
    std::string captured_name;
    std::string captured_value;
    srv.svr.Get("/Users/uid/Views", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.has_header("X-MediaBrowser-Token")) {
            captured_name  = "X-MediaBrowser-Token";
            captured_value = req.get_header_value("X-MediaBrowser-Token");
        }
        res.set_content(R"({"Items":[]})", "application/json");
    });
    srv.start();

    JellyfinSource src("s1", srv.url(), "my-jf-token", "uid");
    src.listAvailableLibraries();
    EXPECT_EQ(captured_name,  "X-MediaBrowser-Token");
    EXPECT_EQ(captured_value, "my-jf-token");
}

TEST(EmbyMeta, SendsXEmbyTokenHeader) {
    TestServer srv;
    std::string captured_name;
    std::string captured_value;
    srv.svr.Get("/Users/uid/Views", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.has_header("X-Emby-Token")) {
            captured_name  = "X-Emby-Token";
            captured_value = req.get_header_value("X-Emby-Token");
        }
        res.set_content(R"({"Items":[]})", "application/json");
    });
    srv.start();

    EmbySource src("s1", srv.url(), "my-emby-token", "uid");
    src.listAvailableLibraries();
    EXPECT_EQ(captured_name,  "X-Emby-Token");
    EXPECT_EQ(captured_value, "my-emby-token");
}
