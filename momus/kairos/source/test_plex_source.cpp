// Tests for PlexSource via a local httplib stub server.
// Covers all sync and browse methods, verifying the Plex-specific JSON shape:
// MediaContainer/Directory/Metadata, ratingKey IDs, tag-based arrays, Guid
// prefix stripping, Media[0].Part[0].file paths, millisecond durations (no
// ticks), intro markers, chapters, and fetchListItems.

#include <gtest/gtest.h>
#include <httplib.h>
#include "source/PlexSource.h"
#include <chrono>
#include <memory>
#include <string>
#include <thread>

// ============================================================================
// TestServer
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
// Canned route registration
// ============================================================================

static void registerRoutes(httplib::Server& s) {

    // ── Library sections ──────────────────────────────────────────────────
    s.Get("/library/sections", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({
            "MediaContainer": {
                "Directory": [
                    {"key":"1","title":"TV Shows","type":"show"},
                    {"key":"2","title":"Movies",  "type":"movie"},
                    {"key":"3","title":"Music",   "type":"artist"},
                    {"key":"4","title":"Photos",  "type":"photo"},
                    {"key":"5","title":"Home Vids","type":"other"}
                ]
            }
        })", "application/json");
    });

    // ── Shows ─────────────────────────────────────────────────────────────
    s.Get("/library/sections/1/all", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({
            "MediaContainer": {
                "Metadata": [{
                    "ratingKey": "show1",
                    "title": "Breaking Bad",
                    "contentRating": "TV-MA",
                    "summary": "A chemistry teacher turns meth cook.",
                    "studio": "AMC",
                    "status": "Ended",
                    "thumb": "/library/metadata/show1/thumb",
                    "art":   "/library/metadata/show1/art",
                    "originallyAvailableAt": "2008-01-20",
                    "year": 2008,
                    "audienceRating": 9.5,
                    "Genre":      [{"tag":"Drama"},{"tag":"Crime"}],
                    "Label":      [{"tag":"favorite"}],
                    "Network":    [{"tag":"AMC"}],
                    "Role":       [{"tag":"Bryan Cranston"},{"tag":"Aaron Paul"}],
                    "Country":    [{"tag":"US"}],
                    "Collection": [{"tag":"Crime Dramas"}],
                    "Guid": [
                        {"id":"imdb://tt0903747"},
                        {"id":"tvdb://81189"},
                        {"id":"tmdb://1396"}
                    ]
                }]
            }
        })", "application/json");
    });

    // ── Movies ────────────────────────────────────────────────────────────
    s.Get("/library/sections/2/all", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({
            "MediaContainer": {
                "Metadata": [
                    {
                        "ratingKey": "movie1",
                        "title": "Inception",
                        "contentRating": "PG-13",
                        "summary": "A thief who steals from dreams.",
                        "tagline": "Your mind is the scene of the crime.",
                        "studio": "WB",
                        "year": 2010,
                        "audienceRating": 8.8,
                        "duration": 7200000,
                        "thumb": "/library/metadata/movie1/thumb",
                        "art":   "/library/metadata/movie1/art",
                        "Director":   [{"tag":"Christopher Nolan"}],
                        "Genre":      [{"tag":"Sci-Fi"},{"tag":"Action"}],
                        "Label":      [{"tag":"arthouse"}],
                        "Role":       [{"tag":"Leonardo DiCaprio"}],
                        "Country":    [{"tag":"US"},{"tag":"UK"}],
                        "Collection": [{"tag":"Nolan Films"}],
                        "Guid": [
                            {"id":"imdb://tt1375666"},
                            {"id":"tmdb://27205"}
                        ],
                        "Media": [{"Part":[{"file":"/media/Inception.mkv"}],"duration":7200000}]
                    },
                    {
                        "ratingKey": "movie2",
                        "title": "Fallback Duration",
                        "Media": [{"Part":[{"file":"/media/movie2.mkv"}],"duration":3600000}]
                    },
                    {
                        "ratingKey": "movie3",
                        "title": "No Path Movie",
                        "Media": []
                    }
                ]
            }
        })", "application/json");
    });

    // ── Episodes ──────────────────────────────────────────────────────────
    s.Get("/library/metadata/show1/allLeaves", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({
            "MediaContainer": {
                "Metadata": [
                    {
                        "ratingKey": "ep1",
                        "grandparentRatingKey": "show1",
                        "parentIndex": 1, "index": 1,
                        "title": "Pilot",
                        "summary": "Walt starts cooking.",
                        "parentTitle": "Season 1",
                        "originallyAvailableAt": "2008-01-20",
                        "thumb": "/library/metadata/ep1/thumb",
                        "duration": 2700000,
                        "absoluteIndex": 1,
                        "Media": [{"Part":[{"file":"/media/s01e01.mkv"}]}]
                    },
                    {
                        "ratingKey": "ep2",
                        "grandparentRatingKey": "show1",
                        "parentIndex": 1, "index": 2,
                        "title": "Cat's in the Bag",
                        "parentTitle": "Season 1",
                        "duration": 2880000,
                        "Media": [{"Part":[{"file":"/media/s01e02.mkv"}]}]
                    },
                    {
                        "ratingKey": "ep3",
                        "grandparentRatingKey": "show1",
                        "parentIndex": 2, "index": 1,
                        "title": "Seven Thirty-Seven",
                        "parentTitle": "Season 2",
                        "duration": 2760000,
                        "Media": [{"Part":[{"file":"/media/s02e01.mkv"}]}]
                    },
                    {
                        "ratingKey": "ep_skip",
                        "grandparentRatingKey": "show1",
                        "parentIndex": 1, "index": 99,
                        "title": "No Path",
                        "Media": []
                    },
                    {
                        "ratingKey": "ep_fallback",
                        "grandparentRatingKey": "show1",
                        "parentIndex": 1, "index": 3,
                        "title": "Duration Fallback",
                        "parentTitle": "Season 1",
                        "Media": [{"Part":[{"file":"/media/s01e03.mkv"}],"duration":3000000}]
                    }
                ]
            }
        })", "application/json");
    });

    // ── Intro markers ─────────────────────────────────────────────────────
    s.Get("/library/metadata/ep1/markers", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({
            "MediaContainer": {
                "Marker": [
                    {"type":"intro",   "startTimeOffset":10000,   "endTimeOffset":90000},
                    {"type":"credits", "startTimeOffset":2600000, "endTimeOffset":2700000}
                ]
            }
        })", "application/json");
    });

    // ── Chapters (/library/metadata/ep1?includeChapters=1) ────────────────
    // httplib matches on path; query string is ignored in route lookup.
    s.Get("/library/metadata/ep1", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({
            "MediaContainer": {
                "Metadata": [{
                    "Chapter": [
                        {"tag":"Opening", "startTimeOffset":0,     "endTimeOffset":10000},
                        {"tag":"Act 1",   "startTimeOffset":10000, "endTimeOffset":900000}
                    ]
                }]
            }
        })", "application/json");
    });

    // ── Browse playlists ──────────────────────────────────────────────────
    s.Get("/playlists", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({
            "MediaContainer": {
                "Metadata": [
                    {"ratingKey":"pl1","title":"Watchlist","leafCount":8}
                ]
            }
        })", "application/json");
    });

    // ── Playlist items — used by browsePlaylistItems and fetchListItems ───
    s.Get("/playlists/pl1/items", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({
            "MediaContainer": {
                "Metadata": [
                    {"ratingKey":"pm1","type":"movie",   "title":"Movie A","duration":7200000},
                    {"ratingKey":"pe1","type":"episode", "title":"Ep X",   "duration":2700000,
                     "grandparentTitle":"Show Z","parentIndex":1,"index":3}
                ]
            }
        })", "application/json");
    });

    // ── Browse collections ────────────────────────────────────────────────
    s.Get("/library/sections/1/collections", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({
            "MediaContainer": {
                "Metadata": [
                    {"ratingKey":"col1","title":"Crime Dramas","childCount":5}
                ]
            }
        })", "application/json");
    });

    // ── Collection items — used by browseCollectionItems and fetchListItems
    s.Get("/library/metadata/col1/children", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({
            "MediaContainer": {
                "Metadata": [
                    {"ratingKey":"cm1","type":"movie",   "title":"BB S1","duration":7200000},
                    {"ratingKey":"ce1","type":"episode", "title":"Ep A", "duration":2700000,
                     "grandparentTitle":"Show B","parentIndex":2,"index":4}
                ]
            }
        })", "application/json");
    });

    // ── fetchListItems empty result ───────────────────────────────────────
    s.Get("/playlists/empty/items", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"MediaContainer":{"Metadata":[]}})", "application/json");
    });

    // ── Error paths ───────────────────────────────────────────────────────
    s.Get("/library/sections/err/all",
          [](const httplib::Request&, httplib::Response& r) { r.status = 500; });
    s.Get("/library/metadata/bad-show/allLeaves",
          [](const httplib::Request&, httplib::Response& r) { r.status = 500; });
    s.Get("/library/metadata/no-markers/markers",
          [](const httplib::Request&, httplib::Response& res) {
              res.set_content(R"({"MediaContainer":{}})", "application/json");
          });
    s.Get("/library/metadata/no-chapters",
          [](const httplib::Request&, httplib::Response& res) {
              res.set_content(R"({"MediaContainer":{"Metadata":[{}]}})", "application/json");
          });
    s.Get("/playlists/err/items",
          [](const httplib::Request&, httplib::Response& r) { r.status = 500; });
}

// ============================================================================
// Fixture
// ============================================================================

class PlexSourceTest : public ::testing::Test {
public:
    static void SetUpTestSuite() {
        srv_ = new TestServer();
        registerRoutes(srv_->svr);
        srv_->start();
    }
    static void TearDownTestSuite() { delete srv_; srv_ = nullptr; }

protected:
    void SetUp() override {
        src_ = std::make_unique<PlexSource>("src1", srv_->url(), "test-token");
    }

    static TestServer*          srv_;
    std::unique_ptr<PlexSource> src_;
};

TestServer* PlexSourceTest::srv_ = nullptr;

// ============================================================================
// listAvailableLibraries
// ============================================================================

TEST_F(PlexSourceTest, ListLibraries_Count) {
    EXPECT_EQ(src_->listAvailableLibraries().size(), 5u);
}

TEST_F(PlexSourceTest, ListLibraries_TypeMapping) {
    const auto libs = src_->listAvailableLibraries();
    EXPECT_EQ(libs[0].type, "show");
    EXPECT_EQ(libs[1].type, "movie");
    EXPECT_EQ(libs[2].type, "music");
    EXPECT_EQ(libs[3].type, "photo");
    EXPECT_EQ(libs[4].type, "mixed"); // "other" falls through to mixed
}

TEST_F(PlexSourceTest, ListLibraries_Fields) {
    const auto libs = src_->listAvailableLibraries();
    EXPECT_EQ(libs[0].external_lib_id, "1");
    EXPECT_EQ(libs[0].name,            "TV Shows");
}

// ============================================================================
// fetchShows
// ============================================================================

TEST_F(PlexSourceTest, FetchShows_CoreFields) {
    const auto shows = src_->fetchShows("1");
    ASSERT_EQ(shows.size(), 1u);
    const auto& s = shows[0];
    EXPECT_EQ(s.show_id,                 "show1");
    EXPECT_EQ(s.title,                   "Breaking Bad");
    EXPECT_EQ(s.content_rating,          "TV-MA");
    EXPECT_EQ(s.overview,                "A chemistry teacher turns meth cook.");
    EXPECT_EQ(s.studio,                  "AMC");
    EXPECT_EQ(s.status,                  "Ended");
    EXPECT_EQ(s.originally_available_at, "2008-01-20");
}

TEST_F(PlexSourceTest, FetchShows_YearAndRating) {
    const auto shows = src_->fetchShows("1");
    ASSERT_FALSE(shows.empty());
    const auto& s = shows[0];
    ASSERT_TRUE(s.year.has_value());
    EXPECT_EQ(*s.year, 2008);
    ASSERT_TRUE(s.audience_rating.has_value());
    EXPECT_NEAR(*s.audience_rating, 9.5f, 0.01f);
}

TEST_F(PlexSourceTest, FetchShows_GuidPrefixStripping) {
    const auto shows = src_->fetchShows("1");
    ASSERT_FALSE(shows.empty());
    const auto& s = shows[0];
    EXPECT_EQ(s.imdb_id, "tt0903747"); // stripped "imdb://"
    EXPECT_EQ(s.tvdb_id, "81189");     // stripped "tvdb://"
    EXPECT_EQ(s.tmdb_id, "1396");      // stripped "tmdb://"
}

TEST_F(PlexSourceTest, FetchShows_TagBasedArrays) {
    const auto shows = src_->fetchShows("1");
    ASSERT_FALSE(shows.empty());
    const auto& s = shows[0];
    EXPECT_EQ(s.genres,      "[\"Drama\",\"Crime\"]");
    EXPECT_EQ(s.labels,      "[\"favorite\"]");
    EXPECT_EQ(s.actors,      "[\"Bryan Cranston\",\"Aaron Paul\"]");
    EXPECT_EQ(s.countries,   "[\"US\"]");
    EXPECT_EQ(s.collections, "[\"Crime Dramas\"]");
}

TEST_F(PlexSourceTest, FetchShows_NetworkFromNetworkArray) {
    const auto shows = src_->fetchShows("1");
    ASSERT_FALSE(shows.empty());
    EXPECT_EQ(shows[0].network, "AMC");
}

TEST_F(PlexSourceTest, FetchShows_ImagePaths) {
    const auto shows = src_->fetchShows("1");
    ASSERT_FALSE(shows.empty());
    EXPECT_EQ(shows[0].thumb, "/library/metadata/show1/thumb");
    EXPECT_EQ(shows[0].art,   "/library/metadata/show1/art");
}

TEST_F(PlexSourceTest, FetchShows_EmptyOnServerError) {
    EXPECT_TRUE(src_->fetchShows("err").empty());
}

// ============================================================================
// fetchMovies
// ============================================================================

TEST_F(PlexSourceTest, FetchMovies_SkipsItemWithEmptyMedia) {
    // movie3 has "Media": [] → file_path empty → skipped; only 2 valid items
    EXPECT_EQ(src_->fetchMovies("2").size(), 2u);
}

TEST_F(PlexSourceTest, FetchMovies_CoreFields) {
    const auto movies = src_->fetchMovies("2");
    ASSERT_FALSE(movies.empty());
    const auto& m = movies[0];
    EXPECT_EQ(m.movie_id,       "movie1");
    EXPECT_EQ(m.title,          "Inception");
    EXPECT_EQ(m.content_rating, "PG-13");
    EXPECT_EQ(m.overview,       "A thief who steals from dreams.");
    EXPECT_EQ(m.tagline,        "Your mind is the scene of the crime.");
    EXPECT_EQ(m.studio,         "WB");
    EXPECT_EQ(m.file_path,      "/media/Inception.mkv");
    ASSERT_TRUE(m.year.has_value());
    EXPECT_EQ(*m.year, 2010);
}

TEST_F(PlexSourceTest, FetchMovies_DurationDirectlyInMs) {
    const auto movies = src_->fetchMovies("2");
    ASSERT_FALSE(movies.empty());
    // Duration is stored as milliseconds directly — no RunTimeTicks conversion
    EXPECT_EQ(movies[0].duration_ms, 7'200'000);
}

TEST_F(PlexSourceTest, FetchMovies_DurationFallsBackToMediaLevel) {
    const auto movies = src_->fetchMovies("2");
    ASSERT_EQ(movies.size(), 2u);
    // movie2 has no top-level duration; falls back to Media[0].duration
    EXPECT_EQ(movies[1].movie_id,    "movie2");
    EXPECT_EQ(movies[1].duration_ms, 3'600'000);
}

TEST_F(PlexSourceTest, FetchMovies_GuidPrefixStripping) {
    const auto movies = src_->fetchMovies("2");
    ASSERT_FALSE(movies.empty());
    EXPECT_EQ(movies[0].imdb_id, "tt1375666");
    EXPECT_EQ(movies[0].tmdb_id, "27205");
}

TEST_F(PlexSourceTest, FetchMovies_DirectorAndActors) {
    const auto movies = src_->fetchMovies("2");
    ASSERT_FALSE(movies.empty());
    EXPECT_EQ(movies[0].director, "Christopher Nolan");
    EXPECT_EQ(movies[0].actors,   "[\"Leonardo DiCaprio\"]");
}

TEST_F(PlexSourceTest, FetchMovies_TagArrayFields) {
    const auto movies = src_->fetchMovies("2");
    ASSERT_FALSE(movies.empty());
    const auto& m = movies[0];
    EXPECT_EQ(m.genres,      "[\"Sci-Fi\",\"Action\"]");
    EXPECT_EQ(m.labels,      "[\"arthouse\"]");
    EXPECT_EQ(m.countries,   "[\"US\",\"UK\"]");
    EXPECT_EQ(m.collections, "[\"Nolan Films\"]");
}

TEST_F(PlexSourceTest, FetchMovies_ImagePaths) {
    const auto movies = src_->fetchMovies("2");
    ASSERT_FALSE(movies.empty());
    EXPECT_EQ(movies[0].thumb, "/library/metadata/movie1/thumb");
    EXPECT_EQ(movies[0].art,   "/library/metadata/movie1/art");
}

TEST_F(PlexSourceTest, FetchMovies_EmptyOnServerError) {
    EXPECT_TRUE(src_->fetchMovies("err").empty());
}

// ============================================================================
// fetchEpisodes
// ============================================================================

TEST_F(PlexSourceTest, FetchEpisodes_SkipsItemWithNoPath) {
    // ep_skip has empty Media → 4 valid episodes
    EXPECT_EQ(src_->fetchEpisodes("show1").size(), 4u);
}

TEST_F(PlexSourceTest, FetchEpisodes_CoreFields) {
    const auto eps = src_->fetchEpisodes("show1");
    ASSERT_FALSE(eps.empty());
    const auto& e = eps[0]; // ep1: S1E1 after sort
    EXPECT_EQ(e.episode_id, "ep1");
    EXPECT_EQ(e.show_id,    "show1");
    EXPECT_EQ(e.title,      "Pilot");
    EXPECT_EQ(e.file_path,  "/media/s01e01.mkv");
    EXPECT_EQ(e.overview,   "Walt starts cooking.");
    EXPECT_EQ(e.season,     1);
    EXPECT_EQ(e.episode,    1);
}

TEST_F(PlexSourceTest, FetchEpisodes_DurationDirectlyInMs) {
    const auto eps = src_->fetchEpisodes("show1");
    EXPECT_EQ(eps[0].duration_ms, 2'700'000);
    EXPECT_EQ(eps[1].duration_ms, 2'880'000);
}

TEST_F(PlexSourceTest, FetchEpisodes_DurationFallsBackToMediaLevel) {
    // ep_fallback (S1E3) has no top-level duration; falls back to Media[0].duration
    const auto eps = src_->fetchEpisodes("show1");
    // Sorted: [ep1(S1E1), ep2(S1E2), ep_fallback(S1E3), ep3(S2E1)]
    ASSERT_GE(eps.size(), 3u);
    EXPECT_EQ(eps[2].episode_id,  "ep_fallback");
    EXPECT_EQ(eps[2].duration_ms, 3'000'000);
}

TEST_F(PlexSourceTest, FetchEpisodes_SeasonNameFromParentTitle) {
    const auto eps = src_->fetchEpisodes("show1");
    ASSERT_GE(eps.size(), 4u);
    EXPECT_EQ(eps[0].season_name, "Season 1");
    EXPECT_EQ(eps[3].season_name, "Season 2");
}

TEST_F(PlexSourceTest, FetchEpisodes_AbsoluteIndex) {
    const auto eps = src_->fetchEpisodes("show1");
    ASSERT_TRUE(eps[0].absolute_index.has_value());
    EXPECT_EQ(*eps[0].absolute_index, 1);
    EXPECT_FALSE(eps[1].absolute_index.has_value()); // ep2 has none
}

TEST_F(PlexSourceTest, FetchEpisodes_AirDate) {
    const auto eps = src_->fetchEpisodes("show1");
    EXPECT_EQ(eps[0].air_date, "2008-01-20");
    EXPECT_EQ(eps[1].air_date, "");
}

TEST_F(PlexSourceTest, FetchEpisodes_ThumbPath) {
    const auto eps = src_->fetchEpisodes("show1");
    EXPECT_EQ(eps[0].thumb, "/library/metadata/ep1/thumb");
    EXPECT_EQ(eps[1].thumb, "");
}

TEST_F(PlexSourceTest, FetchEpisodes_SortedBySeasonThenEpisode) {
    const auto eps = src_->fetchEpisodes("show1");
    ASSERT_EQ(eps.size(), 4u);
    EXPECT_EQ(eps[0].episode_id, "ep1");         // S1E1
    EXPECT_EQ(eps[1].episode_id, "ep2");         // S1E2
    EXPECT_EQ(eps[2].episode_id, "ep_fallback"); // S1E3
    EXPECT_EQ(eps[3].episode_id, "ep3");         // S2E1
}

TEST_F(PlexSourceTest, FetchEpisodes_EmptyOnServerError) {
    EXPECT_TRUE(src_->fetchEpisodes("bad-show").empty());
}

// ============================================================================
// fetchPlaylists — intentionally unimplemented (TODO in source)
// ============================================================================

TEST_F(PlexSourceTest, FetchPlaylists_AlwaysEmpty) {
    EXPECT_TRUE(src_->fetchPlaylists("1").empty());
}

// ============================================================================
// fetchIntroMarkers
// ============================================================================

TEST_F(PlexSourceTest, FetchIntroMarkers_Count) {
    EXPECT_EQ(src_->fetchIntroMarkers("ep1").size(), 2u);
}

TEST_F(PlexSourceTest, FetchIntroMarkers_Fields) {
    const auto markers = src_->fetchIntroMarkers("ep1");
    ASSERT_EQ(markers.size(), 2u);
    EXPECT_EQ(markers[0].source,       "plex_intro");
    EXPECT_EQ(markers[0].chapter_type, "intro");
    EXPECT_EQ(markers[0].start_ms,     10000);
    EXPECT_EQ(markers[0].end_ms,       90000);
    EXPECT_EQ(markers[0].position,     0);
    EXPECT_EQ(markers[1].chapter_type, "credits");
    EXPECT_EQ(markers[1].start_ms,     2600000);
    EXPECT_EQ(markers[1].end_ms,       2700000);
    EXPECT_EQ(markers[1].position,     1);
}

TEST_F(PlexSourceTest, FetchIntroMarkers_EmptyWhenNoMarkerKey) {
    EXPECT_TRUE(src_->fetchIntroMarkers("no-markers").empty());
}

// ============================================================================
// fetchChapters
// ============================================================================

TEST_F(PlexSourceTest, FetchChapters_Count) {
    EXPECT_EQ(src_->fetchChapters("ep1").size(), 2u);
}

TEST_F(PlexSourceTest, FetchChapters_Fields) {
    const auto chapters = src_->fetchChapters("ep1");
    ASSERT_EQ(chapters.size(), 2u);
    EXPECT_EQ(chapters[0].source,       "plex_chapters");
    EXPECT_EQ(chapters[0].chapter_type, "unclassified");
    EXPECT_EQ(chapters[0].title,        "Opening");
    EXPECT_EQ(chapters[0].start_ms,     0);
    EXPECT_EQ(chapters[0].end_ms,       10000);
    EXPECT_EQ(chapters[0].position,     0);
    EXPECT_EQ(chapters[1].title,        "Act 1");
    EXPECT_EQ(chapters[1].start_ms,     10000);
    EXPECT_EQ(chapters[1].end_ms,       900000);
    EXPECT_EQ(chapters[1].position,     1);
}

TEST_F(PlexSourceTest, FetchChapters_EmptyWhenNoChapterKey) {
    EXPECT_TRUE(src_->fetchChapters("no-chapters").empty());
}

// ============================================================================
// fetchListItems
// ============================================================================

TEST_F(PlexSourceTest, FetchListItems_Playlist) {
    const auto result = src_->fetchListItems("pl1", "playlist");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);
    EXPECT_EQ((*result)[0].item_type,   "movie");
    EXPECT_EQ((*result)[0].external_id, "pm1");
    EXPECT_EQ((*result)[1].item_type,   "episode");
    EXPECT_EQ((*result)[1].external_id, "pe1");
}

TEST_F(PlexSourceTest, FetchListItems_Collection) {
    const auto result = src_->fetchListItems("col1", "collection");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);
    EXPECT_EQ((*result)[0].item_type,   "movie");
    EXPECT_EQ((*result)[0].external_id, "cm1");
    EXPECT_EQ((*result)[1].item_type,   "episode");
    EXPECT_EQ((*result)[1].external_id, "ce1");
}

TEST_F(PlexSourceTest, FetchListItems_EmptyVectorOnEmptyResponse) {
    const auto result = src_->fetchListItems("empty", "playlist");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST_F(PlexSourceTest, FetchListItems_NulloptOnServerError) {
    EXPECT_FALSE(src_->fetchListItems("err", "playlist").has_value());
}

// ============================================================================
// Browse methods
// ============================================================================

TEST_F(PlexSourceTest, BrowsePlaylists_CountAndFields) {
    const auto lists = src_->browsePlaylists();
    ASSERT_EQ(lists.size(), 1u);
    EXPECT_EQ(lists[0].id,         "pl1");
    EXPECT_EQ(lists[0].title,      "Watchlist");
    EXPECT_EQ(lists[0].item_count, 8);
}

TEST_F(PlexSourceTest, BrowsePlaylistItems_FieldsForMovieAndEpisode) {
    const auto items = src_->browsePlaylistItems("pl1");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].external_id, "pm1");
    EXPECT_EQ(items[0].item_type,   "movie");
    EXPECT_EQ(items[0].duration_ms, 7'200'000);
    EXPECT_EQ(items[1].external_id, "pe1");
    EXPECT_EQ(items[1].item_type,   "episode");
    EXPECT_EQ(items[1].show_title,  "Show Z");
    EXPECT_EQ(items[1].season,      1);
    EXPECT_EQ(items[1].episode,     3);
}

TEST_F(PlexSourceTest, BrowseCollections_CountAndFields) {
    const auto cols = src_->browseCollections("1");
    ASSERT_EQ(cols.size(), 1u);
    EXPECT_EQ(cols[0].id,         "col1");
    EXPECT_EQ(cols[0].title,      "Crime Dramas");
    EXPECT_EQ(cols[0].item_count, 5);
}

TEST_F(PlexSourceTest, BrowseCollectionItems_FieldsForMovieAndEpisode) {
    const auto items = src_->browseCollectionItems("col1");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].external_id, "cm1");
    EXPECT_EQ(items[0].item_type,   "movie");
    EXPECT_EQ(items[1].external_id, "ce1");
    EXPECT_EQ(items[1].item_type,   "episode");
    EXPECT_EQ(items[1].show_title,  "Show B");
    EXPECT_EQ(items[1].season,      2);
    EXPECT_EQ(items[1].episode,     4);
}

// ============================================================================
// Source identity and auth header
// ============================================================================

TEST(PlexMeta, SourceTypeAndIsSupported) {
    PlexSource src("s1", "http://127.0.0.1:1", "tok");
    EXPECT_EQ(src.sourceType(), "plex");
    EXPECT_TRUE(src.isSupported());
}

TEST(PlexMeta, SendsXPlexTokenHeader) {
    TestServer srv;
    std::string captured;
    srv.svr.Get("/library/sections", [&](const httplib::Request& req, httplib::Response& res) {
        captured = req.get_header_value("X-Plex-Token");
        res.set_content(R"({"MediaContainer":{"Directory":[]}})", "application/json");
    });
    srv.start();

    PlexSource src("s1", srv.url(), "my-plex-token");
    src.listAvailableLibraries();
    EXPECT_EQ(captured, "my-plex-token");
}
