// Tests for PlaylistRepository, focused on smart-playlist refresh (see
// Database.cpp migration 87 and PlaylistRepository::refreshSmart) — matches
// the existing db/test_*_repository.cpp in-memory-DB fixture convention.

#include <gtest/gtest.h>
#include "db/PlaylistRepository.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <ctime>
#include <string>

class PlaylistRepositoryTest : public ::testing::Test {
protected:
    Database db{ ":memory:" };
    PlaylistRepository repo{ db };

    void insertMovie(const std::string& id, const std::string& title, const std::string& genres = "[]",
                      int year = 2020) {
        SQLite::Statement s(db.get(), R"(
            INSERT INTO movie (movie_id, title, content_rating, file_path, duration_ms, year, genres, added_at_source)
            VALUES (?,?,?,?,?,?,?,?)
        )");
        s.bind(1, id); s.bind(2, title); s.bind(3, std::string(""));
        s.bind(4, std::string("/x/") + id + ".mkv"); s.bind(5, int64_t{6000000}); s.bind(6, year);
        s.bind(7, genres); s.bind(8, std::string("local"));
        s.exec();
    }

    void insertShow(const std::string& id, const std::string& title, const std::string& genres = "[]") {
        SQLite::Statement s(db.get(),
            "INSERT INTO show (show_id, title, content_rating, genres) VALUES (?,?,?,?)");
        s.bind(1, id); s.bind(2, title); s.bind(3, std::string("")); s.bind(4, genres);
        s.exec();
    }

    void insertEpisode(const std::string& id, const std::string& show_id, int season, int episode) {
        SQLite::Statement s(db.get(), R"(
            INSERT INTO episode (episode_id, show_id, season, episode, title, file_path, duration_ms)
            VALUES (?,?,?,?,?,?,1800000)
        )");
        s.bind(1, id); s.bind(2, show_id); s.bind(3, season); s.bind(4, episode);
        s.bind(5, id); s.bind(6, std::string("/x/") + id + ".mkv");
        s.exec();
    }

    std::vector<std::pair<std::string, std::string>> playlistItems(const std::string& playlist_id) {
        std::vector<std::pair<std::string, std::string>> out;
        SQLite::Statement q(db.get(),
            "SELECT item_type, item_id FROM playlist_item WHERE playlist_id = ? ORDER BY position");
        q.bind(1, playlist_id);
        while (q.executeStep()) out.push_back({q.getColumn(0).getString(), q.getColumn(1).getString()});
        return out;
    }

    void makeSmart(const std::string& playlist_id, const std::string& smart_type,
                   const std::string& filter_expr, const std::string& smart_sort = "title",
                   int smart_limit = 0) {
        SQLite::Statement s(db.get(), R"(
            UPDATE playlist SET membership='smart', smart_type=?, filter_expr=?, smart_sort=?, smart_limit=?
            WHERE playlist_id = ?
        )");
        s.bind(1, smart_type); s.bind(2, filter_expr); s.bind(3, smart_sort); s.bind(4, smart_limit);
        s.bind(5, playlist_id);
        s.exec();
    }

    void setHomeShelf(const std::string& playlist_id, int home_order = 0, int home_tile_limit = 16,
                       const std::string& active_start = "", const std::string& active_end = "") {
        SQLite::Statement s(db.get(), R"(
            UPDATE playlist SET show_on_home=1, home_order=?, home_tile_limit=?,
                                 home_active_start=?, home_active_end=?
            WHERE playlist_id = ?
        )");
        s.bind(1, home_order); s.bind(2, home_tile_limit);
        s.bind(3, active_start); s.bind(4, active_end);
        s.bind(5, playlist_id);
        s.exec();
    }

    static std::string todayMonthDay() {
        std::time_t now = std::time(nullptr);
        std::tm tmnow{};
        localtime_r(&now, &tmnow);
        char buf[6];
        std::snprintf(buf, sizeof(buf), "%02d-%02d", tmnow.tm_mon + 1, tmnow.tm_mday);
        return buf;
    }

    // A window guaranteed NOT to include today, computed relative to today's
    // month rather than a fixed date, so this test never flakes on a
    // particular day of year.
    static std::pair<std::string, std::string> windowExcludingToday() {
        std::time_t now = std::time(nullptr);
        std::tm tmnow{};
        localtime_r(&now, &tmnow);
        return tmnow.tm_mon < 6 ? std::make_pair(std::string("07-01"), std::string("12-31"))
                                 : std::make_pair(std::string("01-01"), std::string("06-30"));
    }
};

TEST_F(PlaylistRepositoryTest, RefreshSmartMovieResolvesFilterIntoPlaylistItems) {
    insertMovie("m1", "Alpha Movie", R"(["Horror"])");
    insertMovie("m2", "Beta Movie",  R"(["Comedy"])");
    auto playlist_id = repo.create("Horror Night");
    makeSmart(playlist_id, "movie", "genre:Horror");

    int synced = repo.refreshSmart(playlist_id);
    EXPECT_EQ(synced, 1);

    auto items = playlistItems(playlist_id);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].first,  "movie");
    EXPECT_EQ(items[0].second, "m1");
}

// Show-typed smart playlists have no "show" playlist_item — they expand to
// every matching show's episodes in season/episode order (playlist_item's
// item_type is only ever episode/movie — see Database.cpp's CHECK).
TEST_F(PlaylistRepositoryTest, RefreshSmartShowExpandsToEpisodesInOrder) {
    insertShow("s1", "Matching Show", R"(["Horror"])");
    insertShow("s2", "Other Show",    R"(["Comedy"])");
    insertEpisode("e1", "s1", 1, 2);
    insertEpisode("e2", "s1", 1, 1);
    auto playlist_id = repo.create("Horror Shows");
    makeSmart(playlist_id, "show", "genre:Horror");

    int synced = repo.refreshSmart(playlist_id);
    EXPECT_EQ(synced, 2);

    auto items = playlistItems(playlist_id);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].second, "e2"); // season 1 episode 1 before episode 2
    EXPECT_EQ(items[1].second, "e1");
}

TEST_F(PlaylistRepositoryTest, RefreshSmartRespectsLimit) {
    insertMovie("m1", "A", R"(["Horror"])");
    insertMovie("m2", "B", R"(["Horror"])");
    insertMovie("m3", "C", R"(["Horror"])");
    auto playlist_id = repo.create("Capped");
    makeSmart(playlist_id, "movie", "genre:Horror", "title", 2);

    EXPECT_EQ(repo.refreshSmart(playlist_id), 2);
}

TEST_F(PlaylistRepositoryTest, RefreshSmartOverwritesPreviousItemsNotAppends) {
    insertMovie("m1", "Alpha", R"(["Horror"])");
    insertMovie("m2", "Beta",  R"(["Horror"])");
    auto playlist_id = repo.create("Refreshable");
    makeSmart(playlist_id, "movie", "genre:Horror");
    repo.refreshSmart(playlist_id);
    ASSERT_EQ(playlistItems(playlist_id).size(), 2u);

    // Narrow the filter and refresh again — old membership must be replaced,
    // not left stacked alongside the new (smaller) result.
    makeSmart(playlist_id, "movie", "title:Alpha");
    int synced = repo.refreshSmart(playlist_id);
    EXPECT_EQ(synced, 1);
    EXPECT_EQ(playlistItems(playlist_id).size(), 1u);
}

TEST_F(PlaylistRepositoryTest, RefreshSmartThrowsOnStaticPlaylist) {
    auto playlist_id = repo.create("Static One");
    EXPECT_THROW(repo.refreshSmart(playlist_id), std::exception);
}

TEST_F(PlaylistRepositoryTest, RefreshSmartThrowsOnUnknownPlaylist) {
    EXPECT_THROW(repo.refreshSmart("does-not-exist"), std::exception);
}

// ---------------------------------------------------------------------------
// Home shelves (Database.cpp migration 88)
// ---------------------------------------------------------------------------

TEST_F(PlaylistRepositoryTest, ListHomeShelvesIncludesSmartPlaylistWithNoWindow) {
    auto playlist_id = repo.create("Shelf One");
    makeSmart(playlist_id, "movie", "genre:Horror");
    setHomeShelf(playlist_id);

    auto shelves = repo.listHomeShelves();
    ASSERT_EQ(shelves.size(), 1u);
    EXPECT_EQ(shelves[0].playlist_id, playlist_id);
    EXPECT_EQ(shelves[0].filter_expr, "genre:Horror");
}

TEST_F(PlaylistRepositoryTest, ListHomeShelvesExcludesPlaylistNotShownOnHome) {
    auto playlist_id = repo.create("Not A Shelf");
    makeSmart(playlist_id, "movie", "genre:Horror");
    // show_on_home left at its default (0) — never called setHomeShelf.

    EXPECT_TRUE(repo.listHomeShelves().empty());
}

// A shelf needs a filter to hand to getShows/getMovies the way the Home
// page's own built-in shelves work — a static playlist's explicit item list
// has no such representation, so it's excluded even with show_on_home=1.
TEST_F(PlaylistRepositoryTest, ListHomeShelvesExcludesStaticPlaylistEvenIfMarkedShowOnHome) {
    auto playlist_id = repo.create("Static Shelf Attempt");
    setHomeShelf(playlist_id);

    EXPECT_TRUE(repo.listHomeShelves().empty());
}

TEST_F(PlaylistRepositoryTest, ListHomeShelvesIncludesWhenTodayIsInsideWindow) {
    auto playlist_id = repo.create("Always Today");
    makeSmart(playlist_id, "movie", "genre:Horror");
    std::string today = todayMonthDay();
    setHomeShelf(playlist_id, 0, 16, today, today); // single-day window = today

    EXPECT_EQ(repo.listHomeShelves().size(), 1u);
}

TEST_F(PlaylistRepositoryTest, ListHomeShelvesExcludesWhenTodayIsOutsideWindow) {
    auto playlist_id = repo.create("Not Today");
    makeSmart(playlist_id, "movie", "genre:Horror");
    auto [start, end] = windowExcludingToday();
    setHomeShelf(playlist_id, 0, 16, start, end);

    EXPECT_TRUE(repo.listHomeShelves().empty());
}

TEST_F(PlaylistRepositoryTest, ListHomeShelvesOrdersByHomeOrder) {
    auto p1 = repo.create("Second");
    auto p2 = repo.create("First");
    makeSmart(p1, "movie", "genre:Horror");
    makeSmart(p2, "movie", "genre:Comedy");
    setHomeShelf(p1, /*home_order=*/1);
    setHomeShelf(p2, /*home_order=*/0);

    auto shelves = repo.listHomeShelves();
    ASSERT_EQ(shelves.size(), 2u);
    EXPECT_EQ(shelves[0].playlist_id, p2); // home_order=0 first
    EXPECT_EQ(shelves[1].playlist_id, p1);
}

TEST_F(PlaylistRepositoryTest, ListHomeShelvesCarriesTileLimit) {
    auto playlist_id = repo.create("Capped Shelf");
    makeSmart(playlist_id, "show", "genre:Horror");
    setHomeShelf(playlist_id, 0, 8);

    auto shelves = repo.listHomeShelves();
    ASSERT_EQ(shelves.size(), 1u);
    EXPECT_EQ(shelves[0].home_tile_limit, 8);
    EXPECT_EQ(shelves[0].smart_type, "show");
}
