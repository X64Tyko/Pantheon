// Tests for ContentRepository::getWritebackEligibleShowIds/
// getWritebackEligibleMovieIds — the query backing "Writeback All"'s
// per-library/per-source scoping. Plain Database + repository fixture (no
// Router/HTTP needed here; that layer is exercised separately by the
// /api/writeback/all and /api/writeback/status auth-gating tests in
// api/test_writeback_all.cpp).

#include <gtest/gtest.h>
#include "db/ContentRepository.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {
bool contains(const std::vector<std::string>& v, const std::string& id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}
}

class ContentRepositoryWritebackTest : public ::testing::Test {
protected:
    Database         db{ ":memory:" };
    ContentRepository repo{ db };

    void insertShow(const std::string& id, bool match_confirmed) {
        SQLite::Statement s(db.get(),
            "INSERT INTO show (show_id, title, match_confirmed) VALUES (?,?,?)");
        s.bind(1, id); s.bind(2, id + " title"); s.bind(3, match_confirmed ? 1 : 0);
        s.exec();
    }

    void insertMovie(const std::string& id, bool match_confirmed) {
        SQLite::Statement s(db.get(),
            "INSERT INTO movie (movie_id, title, file_path, duration_ms, match_confirmed) VALUES (?,?,?,?,?)");
        s.bind(1, id); s.bind(2, id + " title"); s.bind(3, "/media/" + id + ".mkv");
        s.bind(4, 3600000); s.bind(5, match_confirmed ? 1 : 0);
        s.exec();
    }

    // source_mapping.source_id/library_id are both real foreign keys
    // (media_source/media_library) — seeds both, idempotently, before
    // inserting the mapping row itself so callers don't have to.
    void mapItem(const std::string& item_type, const std::string& kairos_id,
                 const std::string& source_id, const std::string& library_id) {
        { SQLite::Statement s(db.get(),
            "INSERT OR IGNORE INTO media_source (source_id, source_type, display_name, base_url) VALUES (?,?,?,?)");
          s.bind(1, source_id); s.bind(2, "jellyfin"); s.bind(3, source_id + " display"); s.bind(4, "http://x");
          s.exec(); }
        { SQLite::Statement s(db.get(),
            "INSERT OR IGNORE INTO media_library (library_id, source_id, external_lib_id, display_name, library_type) VALUES (?,?,?,?,?)");
          s.bind(1, library_id); s.bind(2, source_id); s.bind(3, library_id + "-ext");
          s.bind(4, library_id + " display"); s.bind(5, "mixed");
          s.exec(); }
        SQLite::Statement s(db.get(), R"(
            INSERT INTO source_mapping (item_type, kairos_id, source_id, library_id, external_id)
            VALUES (?,?,?,?,?)
        )");
        s.bind(1, item_type); s.bind(2, kairos_id); s.bind(3, source_id);
        s.bind(4, library_id); s.bind(5, kairos_id + "-ext");
        s.exec();
    }
};

// ---------------------------------------------------------------------------
// Shows
// ---------------------------------------------------------------------------

TEST_F(ContentRepositoryWritebackTest, Shows_UnfilteredExcludesUnconfirmedMatches) {
    insertShow("show1", true);
    insertShow("show2", false); // unconfirmed — must never appear
    mapItem("show", "show1", "src-a", "lib-a");
    mapItem("show", "show2", "src-a", "lib-a");

    auto ids = repo.getWritebackEligibleShowIds("", "");
    EXPECT_TRUE(contains(ids, "show1"));
    EXPECT_FALSE(contains(ids, "show2"));
}

TEST_F(ContentRepositoryWritebackTest, Shows_LibraryFilterScopesToThatLibraryOnly) {
    insertShow("show1", true);
    insertShow("show2", true);
    mapItem("show", "show1", "src-a", "lib-a");
    mapItem("show", "show2", "src-b", "lib-b");

    auto ids = repo.getWritebackEligibleShowIds("lib-a", "");
    EXPECT_TRUE(contains(ids, "show1"));
    EXPECT_FALSE(contains(ids, "show2"));
}

TEST_F(ContentRepositoryWritebackTest, Shows_SourceFilterScopesToThatSourceOnly) {
    insertShow("show1", true);
    insertShow("show2", true);
    mapItem("show", "show1", "src-a", "lib-a");
    mapItem("show", "show2", "src-b", "lib-b");

    auto ids = repo.getWritebackEligibleShowIds("", "src-b");
    EXPECT_FALSE(contains(ids, "show1"));
    EXPECT_TRUE(contains(ids, "show2"));
}

TEST_F(ContentRepositoryWritebackTest, Shows_CrossSourceMergedItemMatchesOnEitherMapping) {
    // A show linked to two sources via cross-source merge — filtering by
    // either library it belongs to should include it, not just the first.
    insertShow("show1", true);
    mapItem("show", "show1", "src-a", "lib-a");
    mapItem("show", "show1", "src-c", "lib-c");

    EXPECT_TRUE(contains(repo.getWritebackEligibleShowIds("lib-a", ""), "show1"));
    EXPECT_TRUE(contains(repo.getWritebackEligibleShowIds("lib-c", ""), "show1"));
    EXPECT_TRUE(contains(repo.getWritebackEligibleShowIds("", "src-c"), "show1"));
}

// library_id and source_id are deliberately independent scoping axes, not a
// "must be the same mapping row" intersection: library_id picks which items
// are in scope (via ANY of that item's mappings), source_id picks which
// target(s) get written to (via writebackShow/writebackMovie's own
// source_id_filter, exercised in JellyfinPushMetadata's tests and the
// ContentService.cpp routes, not re-tested here). This is intentional, not
// an accident — it's what makes "push my Plex Anime library's items over to
// Jellyfin" (library on one source, target on another) expressible at all,
// which matters given cross-source sync is the whole point of the feature.
TEST_F(ContentRepositoryWritebackTest, Shows_LibraryAndSourceFiltersAreIndependentAxes) {
    insertShow("show1", true); // in lib-a (via src-a) AND linked to src-c (via lib-c)
    insertShow("show2", true); // in lib-a only, never linked to src-c at all
    mapItem("show", "show1", "src-a", "lib-a");
    mapItem("show", "show1", "src-c", "lib-c");
    mapItem("show", "show2", "src-a", "lib-a");

    // show1 satisfies both filters independently (has a lib-a row, has a
    // separate src-c row) even though no single row has both.
    EXPECT_TRUE(contains(repo.getWritebackEligibleShowIds("lib-a", "src-c"), "show1"));
    // show2 has no src-c mapping at all — fails the source filter outright.
    EXPECT_FALSE(contains(repo.getWritebackEligibleShowIds("lib-a", "src-c"), "show2"));
}

// ---------------------------------------------------------------------------
// Movies (mirrors the show coverage above at lighter weight — same query shape)
// ---------------------------------------------------------------------------

TEST_F(ContentRepositoryWritebackTest, Movies_UnfilteredExcludesUnconfirmedMatches) {
    insertMovie("movie1", true);
    insertMovie("movie2", false);
    mapItem("movie", "movie1", "src-a", "lib-a");
    mapItem("movie", "movie2", "src-a", "lib-a");

    auto ids = repo.getWritebackEligibleMovieIds("", "");
    EXPECT_TRUE(contains(ids, "movie1"));
    EXPECT_FALSE(contains(ids, "movie2"));
}

TEST_F(ContentRepositoryWritebackTest, Movies_LibraryAndSourceFiltersScope) {
    insertMovie("movie1", true);
    insertMovie("movie2", true);
    mapItem("movie", "movie1", "src-a", "lib-a");
    mapItem("movie", "movie2", "src-b", "lib-b");

    EXPECT_TRUE(contains(repo.getWritebackEligibleMovieIds("lib-a", ""), "movie1"));
    EXPECT_FALSE(contains(repo.getWritebackEligibleMovieIds("lib-a", ""), "movie2"));
    EXPECT_TRUE(contains(repo.getWritebackEligibleMovieIds("", "src-b"), "movie2"));
    EXPECT_FALSE(contains(repo.getWritebackEligibleMovieIds("", "src-b"), "movie1"));
}
