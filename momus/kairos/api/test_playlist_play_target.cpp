// Tests for resolvePlaylistPlayTarget (PlaylistPlayTargetHelper.cpp) — the
// playlist-VOD-playback resume algorithm backing GET
// /api/playlists/:id/resolve-play-target. Calls the helper directly (same
// pattern as test_list_push_helper.cpp / test_sync_source_list_items.cpp)
// rather than standing up a full httplib::Server + auth session, since the
// route handler itself is just currentUser() + this call.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "api/services/PlaylistPlayTargetHelper.h"
#include "db/Database.h"
#include "db/PlaylistRepository.h"
#include "db/WatchProgressRepository.h"
#include <SQLiteCpp/SQLiteCpp.h>

using json = nlohmann::json;

class PlaylistPlayTargetTest : public ::testing::Test {
protected:
    Database db{ ":memory:" };
    PlaylistRepository repo{ db };
    static constexpr const char* kUser = "user-1";

    void SetUp() override {
        SQLite::Statement s(db.get(),
            "INSERT INTO user (user_id, username, password_hash, role, created_at) VALUES (?,?,?,?,?)");
        s.bind(1, kUser); s.bind(2, std::string("tester")); s.bind(3, std::string("x"));
        s.bind(4, std::string("viewer")); s.bind(5, int64_t{1000});
        s.exec();
    }

    void insertMovie(const std::string& id, const std::string& title) {
        SQLite::Statement s(db.get(), R"(
            INSERT INTO movie (movie_id, title, content_rating, file_path, duration_ms, year, genres, added_at_source)
            VALUES (?,?,?,?,?,?,?,?)
        )");
        s.bind(1, id); s.bind(2, title); s.bind(3, std::string(""));
        s.bind(4, std::string("/x/") + id + ".mkv"); s.bind(5, int64_t{6000000}); s.bind(6, 2020);
        s.bind(7, std::string("[]")); s.bind(8, std::string("local"));
        s.exec();
    }

    void insertShow(const std::string& id, const std::string& title) {
        SQLite::Statement s(db.get(),
            "INSERT INTO show (show_id, title, content_rating, genres) VALUES (?,?,?,?)");
        s.bind(1, id); s.bind(2, title); s.bind(3, std::string("")); s.bind(4, std::string("[]"));
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

    void setProgress(const std::string& content_type, const std::string& content_id,
                      int64_t position_ms, int64_t duration_ms) {
        WatchProgressRepository(db).upsert(kUser, content_type, content_id, position_ms, duration_ms,
                                            /*updated_at=*/1000, /*completed=*/position_ms >= duration_ms);
    }

    json resolve(const std::string& playlist_id) {
        httplib::Response res;
        resolvePlaylistPlayTarget(res, playlist_id, kUser, db);
        return json::parse(res.body);
    }
};

TEST_F(PlaylistPlayTargetTest, EmptyPlaylistReturnsNull) {
    auto playlist_id = repo.create("Empty");
    EXPECT_TRUE(resolve(playlist_id).is_null());
}

TEST_F(PlaylistPlayTargetTest, MissingPlaylistReturnsNull) {
    EXPECT_TRUE(resolve("no-such-playlist").is_null());
}

TEST_F(PlaylistPlayTargetTest, NoWatchStateStartsAtFirstItem) {
    insertMovie("m1", "Movie One");
    insertMovie("m2", "Movie Two");
    auto playlist_id = repo.create("List");
    repo.addItem(playlist_id, "movie", "m1");
    repo.addItem(playlist_id, "movie", "m2");

    auto r = resolve(playlist_id);
    EXPECT_EQ(r["kind"], "movie");
    EXPECT_EQ(r["id"], "m1");
    EXPECT_EQ(r["position_ms"], 0);
}

TEST_F(PlaylistPlayTargetTest, ResumesInProgressItemAtItsPosition) {
    insertMovie("m1", "Movie One");
    insertMovie("m2", "Movie Two");
    auto playlist_id = repo.create("List");
    repo.addItem(playlist_id, "movie", "m1");
    repo.addItem(playlist_id, "movie", "m2");
    setProgress("movie", "m1", 30000, 6000000);

    auto r = resolve(playlist_id);
    EXPECT_EQ(r["id"], "m1");
    EXPECT_EQ(r["position_ms"], 30000);
}

TEST_F(PlaylistPlayTargetTest, SkipsCompletedItemsToFirstUnwatched) {
    insertShow("s1", "Show");
    insertEpisode("e1", "s1", 1, 1);
    insertEpisode("e2", "s1", 1, 2);
    insertEpisode("e3", "s1", 1, 3);
    auto playlist_id = repo.create("List");
    repo.addItem(playlist_id, "episode", "e1");
    repo.addItem(playlist_id, "episode", "e2");
    repo.addItem(playlist_id, "episode", "e3");
    // e1 fully watched (position >= duration); e2 untouched.
    setProgress("episode", "e1", 1800000, 1800000);

    auto r = resolve(playlist_id);
    EXPECT_EQ(r["id"], "e2");
    EXPECT_EQ(r["position_ms"], 0);
}

TEST_F(PlaylistPlayTargetTest, AllCompletedRestartsFromFirstItem) {
    insertMovie("m1", "Movie One");
    insertMovie("m2", "Movie Two");
    auto playlist_id = repo.create("List");
    repo.addItem(playlist_id, "movie", "m1");
    repo.addItem(playlist_id, "movie", "m2");
    setProgress("movie", "m1", 6000000, 6000000);
    setProgress("movie", "m2", 6000000, 6000000);

    auto r = resolve(playlist_id);
    EXPECT_EQ(r["id"], "m1");
    EXPECT_EQ(r["position_ms"], 0);
}

// Regression guard for the composite (content_type, content_id) key: a movie
// and an episode sharing the same raw id string must resolve independently
// rather than one's watch_progress row leaking into the other's lookup.
TEST_F(PlaylistPlayTargetTest, SameIdAcrossTypesDoesNotCrossMatch) {
    insertMovie("shared-id", "The Movie");
    insertShow("s1", "Show");
    insertEpisode("shared-id", "s1", 1, 1);
    auto playlist_id = repo.create("List");
    repo.addItem(playlist_id, "movie", "shared-id");
    repo.addItem(playlist_id, "episode", "shared-id");
    // Only the movie is complete; the episode with the same raw id is not.
    setProgress("movie", "shared-id", 6000000, 6000000);

    auto r = resolve(playlist_id);
    EXPECT_EQ(r["kind"], "episode");
    EXPECT_EQ(r["id"], "shared-id");
    EXPECT_EQ(r["position_ms"], 0);
}
