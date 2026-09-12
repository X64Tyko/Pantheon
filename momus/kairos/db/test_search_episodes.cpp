// Tests for ContentRepository::searchEpisodes's upgrade (see PlaylistPlayTargetHelper
// and FilterExpr's `playlist:<id>` field for the sibling Show/Movie mechanism) —
// backing the Library page's "Include Episodes" toggle: total-count pagination,
// playlist membership scoping, and "Playlist Order" sort.

#include <gtest/gtest.h>
#include "db/ContentRepository.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <string>

class SearchEpisodesTest : public ::testing::Test {
protected:
    Database          db{ ":memory:" };
    ContentRepository repo{ db };

    void insertShow(const std::string& id, const std::string& title) {
        SQLite::Statement s(db.get(),
            "INSERT INTO show (show_id, title, content_rating, genres) VALUES (?,?,?,?)");
        s.bind(1, id); s.bind(2, title); s.bind(3, std::string("")); s.bind(4, std::string("[]"));
        s.exec();
    }

    void insertEpisode(const std::string& id, const std::string& show_id, int season, int episode,
                        const std::string& title = "") {
        SQLite::Statement s(db.get(), R"(
            INSERT INTO episode (episode_id, show_id, season, episode, title, file_path, duration_ms)
            VALUES (?,?,?,?,?,?,1800000)
        )");
        s.bind(1, id); s.bind(2, show_id); s.bind(3, season); s.bind(4, episode);
        s.bind(5, title.empty() ? id : title); s.bind(6, std::string("/x/") + id + ".mkv");
        s.exec();
    }

    void insertPlaylist(const std::string& playlist_id) {
        SQLite::Statement s(db.get(),
            "INSERT INTO playlist (playlist_id, title, source, mode) VALUES (?,?,'custom','sequential')");
        s.bind(1, playlist_id); s.bind(2, playlist_id);
        s.exec();
    }

    void insertPlaylistItem(const std::string& playlist_id, int position,
                             const std::string& item_type, const std::string& item_id) {
        SQLite::Statement s(db.get(),
            "INSERT INTO playlist_item (playlist_id, position, item_type, item_id) VALUES (?,?,?,?)");
        s.bind(1, playlist_id); s.bind(2, position); s.bind(3, item_type); s.bind(4, item_id);
        s.exec();
    }
};

TEST_F(SearchEpisodesTest, ReturnsTotalAlongsideItemsForPagination) {
    insertShow("s1", "Show One");
    for (int i = 1; i <= 5; ++i) insertEpisode("e" + std::to_string(i), "s1", 1, i);

    EpisodeSearchParams p; p.limit = 2;
    auto r = repo.searchEpisodes(p);

    EXPECT_EQ(r.total, 5);
    EXPECT_EQ(r.items.size(), 2u);
}

TEST_F(SearchEpisodesTest, PlaylistIdScopesToOnlyThatPlaylistsEpisodeMembers) {
    insertShow("s1", "Show One");
    insertEpisode("e1", "s1", 1, 1);
    insertEpisode("e2", "s1", 1, 2);
    insertPlaylist("pl1");
    insertPlaylistItem("pl1", 0, "episode", "e1");

    EpisodeSearchParams p; p.playlist_id = "pl1"; p.limit = 50;
    auto r = repo.searchEpisodes(p);

    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].episode_id, "e1");
    EXPECT_EQ(r.total, 1);
}

TEST_F(SearchEpisodesTest, PlaylistOrderSortsByPlaylistPositionNotSeasonEpisode) {
    insertShow("s1", "Show One");
    insertEpisode("e1", "s1", 1, 1);
    insertEpisode("e2", "s1", 1, 2);
    insertPlaylist("pl1");
    // Deliberately reversed vs season/episode order.
    insertPlaylistItem("pl1", 0, "episode", "e2");
    insertPlaylistItem("pl1", 1, "episode", "e1");

    EpisodeSearchParams p;
    p.playlist_id = "pl1"; p.sort = "playlist_order"; p.limit = 50;
    auto r = repo.searchEpisodes(p);

    ASSERT_EQ(r.items.size(), 2u);
    EXPECT_EQ(r.items[0].episode_id, "e2");
    EXPECT_EQ(r.items[1].episode_id, "e1");
}

// Regression: same class of bug as ContentRepository's Show/Movie searches —
// see PlaylistOrderSortWithoutPlaylistIdFallsBackInsteadOfThrowing in
// test_filter_expr.cpp.
TEST_F(SearchEpisodesTest, PlaylistOrderSortWithoutPlaylistIdFallsBackInsteadOfThrowing) {
    insertShow("s1", "Show One");
    insertEpisode("e1", "s1", 1, 1);

    EpisodeSearchParams p;
    p.sort = "playlist_order"; p.limit = 50; // no playlist_id set
    EXPECT_NO_THROW({
        auto r = repo.searchEpisodes(p);
        EXPECT_EQ(r.items.size(), 1u);
    });
}

TEST_F(SearchEpisodesTest, DefaultSortIsShowTitleThenSeasonThenEpisode) {
    insertShow("s1", "B Show");
    insertShow("s2", "A Show");
    insertEpisode("e1", "s1", 1, 2);
    insertEpisode("e2", "s1", 1, 1);
    insertEpisode("e3", "s2", 1, 1);

    EpisodeSearchParams p; p.limit = 50;
    auto r = repo.searchEpisodes(p);

    ASSERT_EQ(r.items.size(), 3u);
    EXPECT_EQ(r.items[0].episode_id, "e3"); // "A Show" first
    EXPECT_EQ(r.items[1].episode_id, "e2"); // "B Show" S1E1 before S1E2
    EXPECT_EQ(r.items[2].episode_id, "e1");
}
