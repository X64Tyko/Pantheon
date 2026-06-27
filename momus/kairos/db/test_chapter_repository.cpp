#include <gtest/gtest.h>
#include "db/ChapterRepository.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ChapterRepositoryTest : public ::testing::Test {
protected:
    Database          db{ ":memory:" };
    ChapterRepository repo{ db };

    void insertEpisode(const std::string& id) {
        SQLite::Statement s(db.get(),
            "INSERT INTO show (show_id, title) VALUES ('s1','Show') ON CONFLICT DO NOTHING");
        s.exec();
        SQLite::Statement e(db.get(),
            "INSERT INTO episode (episode_id, show_id, season, episode, title,"
            " file_path, duration_ms) VALUES (?,?,?,?,?,?,?)");
        e.bind(1, id); e.bind(2, "s1"); e.bind(3, 1); e.bind(4, 1);
        e.bind(5, "Ep"); e.bind(6, "/ep.mkv"); e.bind(7, 1440000);
        e.exec();
    }

    // Build a minimal Chapter for use in syncChapters calls.
    Chapter makeChapter(const std::string& type, int64_t start, int64_t end, int pos) {
        Chapter c;
        c.chapter_type = type;
        c.start_ms     = start;
        c.end_ms       = end;
        c.position     = pos;
        return c;
    }
};

// ---------------------------------------------------------------------------
// Basic CRUD
// ---------------------------------------------------------------------------

TEST_F(ChapterRepositoryTest, GetReturnsEmptyForNewMedia) {
    auto chapters = repo.get("episode", "ep_unknown");
    EXPECT_TRUE(chapters.empty());
}

TEST_F(ChapterRepositoryTest, CreateManualChapterIsLockedAndReturnsId) {
    insertEpisode("ep1");
    json body = {
        {"chapter_type", "intro"},
        {"title",        "Opening"},
        {"start_ms",     0},
        {"end_ms",       90000},
        {"position",     0}
    };
    std::string id = repo.create("episode", "ep1", body);
    EXPECT_FALSE(id.empty());

    auto chapters = repo.get("episode", "ep1");
    ASSERT_EQ(chapters.size(), 1u);
    EXPECT_EQ(chapters[0].chapter_id,   id);
    EXPECT_EQ(chapters[0].chapter_type, "intro");
    EXPECT_EQ(chapters[0].title,        "Opening");
    EXPECT_EQ(chapters[0].start_ms,     0);
    EXPECT_EQ(chapters[0].end_ms,       90000);
    EXPECT_EQ(chapters[0].source,       "manual");
    EXPECT_TRUE(chapters[0].locked);
}

TEST_F(ChapterRepositoryTest, CreateDefaultsToUnclassifiedWhenTypeOmitted) {
    insertEpisode("ep1");
    std::string id = repo.create("episode", "ep1", json{{"start_ms", 0}, {"end_ms", 1000}});
    auto chapters  = repo.get("episode", "ep1");
    ASSERT_EQ(chapters.size(), 1u);
    EXPECT_EQ(chapters[0].chapter_type, "unclassified");
}

TEST_F(ChapterRepositoryTest, UpdateSetsLockedAndChangesFields) {
    insertEpisode("ep1");
    std::string id = repo.create("episode", "ep1",
        json{{"chapter_type","unclassified"},{"start_ms",0},{"end_ms",1000}});

    // Force locked=0 to simulate an auto-sourced chapter
    SQLite::Statement u(db.get(), "UPDATE chapter SET locked=0 WHERE chapter_id=?");
    u.bind(1, id); u.exec();

    repo.update(id, json{{"chapter_type","intro"},{"title","OP"}});

    auto chapters = repo.get("episode", "ep1");
    ASSERT_EQ(chapters.size(), 1u);
    EXPECT_EQ(chapters[0].chapter_type, "intro");
    EXPECT_EQ(chapters[0].title,        "OP");
    EXPECT_TRUE(chapters[0].locked) << "update must set locked=1";
}

TEST_F(ChapterRepositoryTest, RemoveDeletesChapter) {
    insertEpisode("ep1");
    std::string id = repo.create("episode", "ep1",
        json{{"start_ms", 0}, {"end_ms", 1000}});
    repo.remove(id);
    EXPECT_TRUE(repo.get("episode", "ep1").empty());
}

// ---------------------------------------------------------------------------
// syncChapters — source-scoped replace-unlocked logic
// ---------------------------------------------------------------------------

TEST_F(ChapterRepositoryTest, SyncChaptersInsertsNewRows) {
    insertEpisode("ep1");
    std::vector<Chapter> chapters = {
        makeChapter("intro", 0, 90000, 0),
        makeChapter("credits", 1300000, 1380000, 1)
    };
    repo.syncChapters("episode", "ep1", "plex_intro", chapters);

    auto result = repo.get("episode", "ep1");
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].chapter_type, "intro");
    EXPECT_EQ(result[0].source,       "plex_intro");
    EXPECT_FALSE(result[0].locked);
    EXPECT_EQ(result[1].chapter_type, "credits");
}

TEST_F(ChapterRepositoryTest, SyncChaptersReplacesUnlockedRowsForSameSource) {
    insertEpisode("ep1");
    repo.syncChapters("episode", "ep1", "file",
        { makeChapter("unclassified", 0, 1000, 0),
          makeChapter("unclassified", 1000, 2000, 1) });

    // Re-sync with a different set — old unlocked rows should be gone.
    repo.syncChapters("episode", "ep1", "file",
        { makeChapter("unclassified", 500, 1500, 0) });

    auto result = repo.get("episode", "ep1");
    ASSERT_EQ(result.size(), 1u) << "stale unlocked rows must be deleted";
    EXPECT_EQ(result[0].start_ms, 500);
}

TEST_F(ChapterRepositoryTest, SyncChaptersPreservesLockedRows) {
    insertEpisode("ep1");

    // Create a manual (locked) chapter via the public API.
    repo.create("episode", "ep1",
        json{{"chapter_type","ad_break"},{"start_ms",500},{"end_ms",1000}});

    // Sync file chapters — locked row must survive even though it's the same source scope.
    repo.syncChapters("episode", "ep1", "manual",
        { makeChapter("chapter", 0, 500, 0) });

    auto result = repo.get("episode", "ep1");
    // Both the locked manual chapter and the new sync row should exist.
    EXPECT_GE(result.size(), 1u);
    bool foundLocked = false;
    for (const auto& c : result)
        if (c.chapter_type == "ad_break" && c.locked) foundLocked = true;
    EXPECT_TRUE(foundLocked) << "locked manual chapter must survive syncChapters";
}

TEST_F(ChapterRepositoryTest, SyncChaptersDoesNotAffectOtherSources) {
    insertEpisode("ep1");
    repo.syncChapters("episode", "ep1", "plex_intro",
        { makeChapter("intro", 0, 90000, 0) });
    repo.syncChapters("episode", "ep1", "file",
        { makeChapter("unclassified", 0, 90000, 0) });

    // Re-sync plex_intro only — file chapters must be untouched.
    repo.syncChapters("episode", "ep1", "plex_intro", {});

    auto result = repo.get("episode", "ep1");
    ASSERT_EQ(result.size(), 1u) << "file chapter must remain after plex_intro sync";
    EXPECT_EQ(result[0].source, "file");
}

TEST_F(ChapterRepositoryTest, SyncChaptersWorksForMovies) {
    SQLite::Statement m(db.get(),
        "INSERT INTO movie (movie_id, title, file_path, duration_ms)"
        " VALUES ('mov1','Film','/film.mkv',7200000)");
    m.exec();

    repo.syncChapters("movie", "mov1", "file",
        { makeChapter("chapter", 0,       3600000, 0),
          makeChapter("chapter", 3600000, 7200000, 1) });

    auto result = repo.get("movie", "mov1");
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].media_type, "movie");
    EXPECT_EQ(result[0].media_id,   "mov1");
}

// ---------------------------------------------------------------------------
// toJson serialisation
// ---------------------------------------------------------------------------

TEST_F(ChapterRepositoryTest, ToJsonContainsAllFields) {
    Chapter c;
    c.chapter_id   = "abc";
    c.media_type   = "episode";
    c.media_id     = "ep1";
    c.chapter_type = "intro";
    c.title        = "Opening";
    c.start_ms     = 0;
    c.end_ms       = 90000;
    c.position     = 0;
    c.source       = "plex_intro";
    c.locked       = true;

    json j = ChapterRepository::toJson(c);
    EXPECT_EQ(j["chapter_id"],   "abc");
    EXPECT_EQ(j["media_type"],   "episode");
    EXPECT_EQ(j["chapter_type"], "intro");
    EXPECT_EQ(j["start_ms"],     0);
    EXPECT_EQ(j["end_ms"],       90000);
    EXPECT_EQ(j["source"],       "plex_intro");
    EXPECT_EQ(j["locked"],       true);
}

TEST_F(ChapterRepositoryTest, ToJsonArrayWrapsMultipleChapters) {
    std::vector<Chapter> chapters(3);
    chapters[0].chapter_id = "a";
    chapters[1].chapter_id = "b";
    chapters[2].chapter_id = "c";
    json arr = ChapterRepository::toJson(chapters);
    ASSERT_TRUE(arr.is_array());
    EXPECT_EQ(arr.size(), 3u);
}
