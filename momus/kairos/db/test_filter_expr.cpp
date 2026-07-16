#include <gtest/gtest.h>
#include "db/FilterExpr.h"
#include "db/ContentRepository.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <string>

// ---------------------------------------------------------------------------
// Pure compiler tests — no DB involved, just checking the SQL fragment shape
// for entity-aware / structural cases that are easy to get wrong silently.
// ---------------------------------------------------------------------------

TEST(FilterExprCompileTest, DirectorOnShowCompilesToConstantFalse) {
    auto r = compileFilterExpr("director:Nolan", FilterEntity::Show, "s");
    EXPECT_EQ(r.sql, "1=0");
}

TEST(FilterExprCompileTest, DirectorOnMovieCompilesToRealColumnCheck) {
    auto r = compileFilterExpr("director:Nolan", FilterEntity::Movie, "m");
    EXPECT_NE(r.sql.find("m.director"), std::string::npos);
    ASSERT_EQ(r.binds.size(), 1u);
    EXPECT_EQ(r.binds[0], "Nolan");
}

TEST(FilterExprCompileTest, ResolutionOnShowUsesExistsSubqueryAgainstEpisodes) {
    auto r = compileFilterExpr("resolution:4K", FilterEntity::Show, "s");
    EXPECT_NE(r.sql.find("EXISTS"), std::string::npos);
    EXPECT_NE(r.sql.find("episode"), std::string::npos);
    EXPECT_NE(r.sql.find("resolution_label"), std::string::npos);
}

TEST(FilterExprCompileTest, EmptyFilterCompilesToTautology) {
    auto r = compileFilterExpr("", FilterEntity::Show, "s");
    EXPECT_EQ(r.sql, "1=1");
    EXPECT_TRUE(r.binds.empty());
}

TEST(FilterExprCompileTest, UnknownFieldFallsBackToFuzzyWordNotError) {
    auto r = compileFilterExpr("notarealfield:foo", FilterEntity::Show, "s");
    // Falls back to a bare word — some OR-across-searchable-columns fragment,
    // not a crash/empty/no-op.
    EXPECT_NE(r.sql.find("LIKE"), std::string::npos);
}

TEST(FilterExprCompileTest, AddedInLastShorthandParsesDaysUnit) {
    auto r = compileFilterExpr("added:<30d", FilterEntity::Movie, "m");
    EXPECT_NE(r.sql.find("added_at"), std::string::npos);
    ASSERT_EQ(r.binds.size(), 1u);
    EXPECT_EQ(r.binds[0], "30");
}

// Regression: movies have no `network` column (only shows do) — compiling a
// network clause against Movie used to emit `m.network = ?`, which throws
// "no such column" and (via Promise.all in LibraryStore.fetch()) silently
// discarded the whole request, leaving stale/unfiltered results on screen
// that looked exactly like "the filter did nothing."
TEST(FilterExprCompileTest, NetworkOnMovieCompilesToConstantFalseNotRealColumn) {
    auto r = compileFilterExpr("network:Disney", FilterEntity::Movie, "m");
    EXPECT_EQ(r.sql, "1=0");
}

TEST(FilterExprCompileTest, NetworkOnShowCompilesToRealColumnCheck) {
    auto r = compileFilterExpr("network:contains:Disney", FilterEntity::Show, "s");
    // Malformed op-in-value (no wildcard marker) falls back to the field's
    // default op ('is') rather than a literal "contains:Disney" value.
    EXPECT_NE(r.sql.find("s.network"), std::string::npos);
}

// Regression: contains/begins_with/ends_with/does_not_contain have no
// comparator-symbol equivalent (only gt/gte/lt/lte do), so without the
// wildcard-marker convention below they'd compile identically to a plain
// 'is' exact match — i.e. picking "contains" in the rule builder silently
// became an exact match once the string reached the backend.
TEST(FilterExprCompileTest, WildcardContainsProducesLikeClause) {
    auto r = compileFilterExpr("network:*Disney*", FilterEntity::Show, "s");
    EXPECT_NE(r.sql.find("LIKE '%' || ? || '%'"), std::string::npos);
    ASSERT_EQ(r.binds.size(), 1u);
    EXPECT_EQ(r.binds[0], "Disney");
}

TEST(FilterExprCompileTest, WildcardBeginsWithProducesPrefixLike) {
    auto r = compileFilterExpr("title:Star*", FilterEntity::Show, "s");
    EXPECT_NE(r.sql.find("LIKE ? || '%'"), std::string::npos);
    ASSERT_EQ(r.binds.size(), 1u);
    EXPECT_EQ(r.binds[0], "Star");
}

TEST(FilterExprCompileTest, WildcardEndsWithProducesSuffixLike) {
    auto r = compileFilterExpr("title:*Wars", FilterEntity::Show, "s");
    EXPECT_NE(r.sql.find("LIKE '%' || ?"), std::string::npos);
    ASSERT_EQ(r.binds.size(), 1u);
    EXPECT_EQ(r.binds[0], "Wars");
}

TEST(FilterExprCompileTest, NegatedWildcardContainsProducesDoesNotContain) {
    auto r = compileFilterExpr("-network:*Disney*", FilterEntity::Show, "s");
    EXPECT_NE(r.sql.find("NOT LIKE"), std::string::npos);
}

TEST(FilterExprCompileTest, QuotedValueWithLiteralAsteriskIsNotTreatedAsWildcard) {
    auto r = compileFilterExpr("title:\"*NSYNC\"", FilterEntity::Show, "s");
    // Exact 'is' match on the literal string, asterisk included — not
    // parsed as an ends_with wildcard.
    ASSERT_EQ(r.binds.size(), 1u);
    EXPECT_EQ(r.binds[0], "*NSYNC");
}

// ---------------------------------------------------------------------------
// Integration tests — real in-memory DB via ContentRepository, matching the
// existing db/test_*_repository.cpp fixture convention.
// ---------------------------------------------------------------------------

class FilterExprIntegrationTest : public ::testing::Test {
protected:
    Database         db{ ":memory:" };
    ContentRepository repo{ db };

    void insertMovie(const std::string& id, const std::string& title, const std::string& genres,
                      int year, double audience_rating, int64_t duration_ms,
                      const std::string& resolution_label = "", int64_t added_at = 0,
                      const std::string& director = "") {
        SQLite::Statement s(db.get(), R"(
            INSERT INTO movie (movie_id, title, content_rating, file_path, duration_ms, year,
                                genres, audience_rating, resolution_label, added_at, added_at_source, director)
            VALUES (?,?,?,?,?,?,?,?,?,?,?,?)
        )");
        s.bind(1, id); s.bind(2, title); s.bind(3, std::string(""));
        s.bind(4, std::string("/x/") + id + ".mkv"); s.bind(5, duration_ms); s.bind(6, year);
        s.bind(7, genres); s.bind(8, audience_rating); s.bind(9, resolution_label);
        if (added_at > 0) s.bind(10, added_at); else s.bind(10);
        s.bind(11, std::string("local")); s.bind(12, director);
        s.exec();
    }

    void insertShow(const std::string& id, const std::string& title, const std::string& genres, int year,
                     const std::string& network = "") {
        SQLite::Statement s(db.get(), R"(
            INSERT INTO show (show_id, title, content_rating, genres, year, network) VALUES (?,?,?,?,?,?)
        )");
        s.bind(1, id); s.bind(2, title); s.bind(3, std::string("")); s.bind(4, genres); s.bind(5, year);
        s.bind(6, network);
        s.exec();
    }

    void insertEpisode(const std::string& id, const std::string& show_id, const std::string& resolution_label) {
        SQLite::Statement s(db.get(), R"(
            INSERT INTO episode (episode_id, show_id, season, episode, title, file_path, duration_ms, resolution_label)
            VALUES (?,?,1,1,?,?,1800000,?)
        )");
        s.bind(1, id); s.bind(2, show_id); s.bind(3, id); s.bind(4, std::string("/x/") + id + ".mkv"); s.bind(5, resolution_label);
        s.exec();
    }

    void insertSource(const std::string& source_id, const std::string& source_type) {
        SQLite::Statement s(db.get(),
            "INSERT INTO media_source (source_id, source_type, display_name, base_url) VALUES (?,?,?,?)");
        s.bind(1, source_id); s.bind(2, source_type); s.bind(3, source_id); s.bind(4, "http://x");
        s.exec();
    }

    void mapToSource(const std::string& item_type, const std::string& kairos_id, const std::string& source_id) {
        SQLite::Statement s(db.get(), R"(
            INSERT INTO source_mapping (item_type, kairos_id, source_id, external_id) VALUES (?,?,?,?)
        )");
        s.bind(1, item_type); s.bind(2, kairos_id); s.bind(3, source_id); s.bind(4, kairos_id);
        s.exec();
    }

    ShowListResult  searchShows(const std::string& filter, const std::string& match = "all") {
        ShowSearchParams p; p.filter = filter; p.limit = 50; return repo.searchShows(p);
    }
    MovieListResult searchMovies(const std::string& filter) {
        MovieSearchParams p; p.filter = filter; p.limit = 50; return repo.searchMovies(p);
    }
};

TEST_F(FilterExprIntegrationTest, GenreIsAndIsNotAreExactComplements) {
    insertMovie("m1", "A", R"(["Comedy"])", 2020, 5, 6000000);
    insertMovie("m2", "B", R"(["Drama"])",  2020, 5, 6000000);
    EXPECT_EQ(searchMovies("genre:Comedy").total, 1);
    EXPECT_EQ(searchMovies("-genre:Comedy").total, 1);
}

TEST_F(FilterExprIntegrationTest, YearComparisonOperators) {
    insertMovie("m1", "Old",   "[]", 1990, 5, 6000000);
    insertMovie("m2", "New",   "[]", 2020, 5, 6000000);
    EXPECT_EQ(searchMovies("year:<2000").total, 1);
    EXPECT_EQ(searchMovies("year:>2000").total, 1);
    EXPECT_EQ(searchMovies("year:1990").total, 1);
}

TEST_F(FilterExprIntegrationTest, ImplicitAndBetweenTwoClauses) {
    insertMovie("m1", "A", R"(["Comedy"])", 2020, 5, 6000000);
    insertMovie("m2", "B", R"(["Comedy"])", 1990, 5, 6000000);
    EXPECT_EQ(searchMovies("genre:Comedy AND year:>2000").total, 1);
    EXPECT_EQ(searchMovies("genre:Comedy year:>2000").total, 1); // implicit AND, no keyword
}

TEST_F(FilterExprIntegrationTest, OrIsARealUnion) {
    insertMovie("m1", "A", R"(["Comedy"])", 2020, 5, 6000000);
    insertMovie("m2", "B", R"(["Drama"])",  2020, 5, 6000000);
    insertMovie("m3", "C", R"(["Horror"])", 2020, 5, 6000000);
    EXPECT_EQ(searchMovies("genre:Comedy OR genre:Drama").total, 2);
}

TEST_F(FilterExprIntegrationTest, NestedGroupComposesWithOuterOr) {
    insertMovie("m1", "A", R"(["Comedy"])", 2020, 5, 6000000); // matches group
    insertMovie("m2", "B", R"(["Comedy"])", 1990, 5, 6000000); // fails group (year)
    insertMovie("m3", "C", R"(["Drama"])",  1990, 5, 6000000); // matches via outer OR
    auto r = searchMovies("(genre:Comedy AND year:>2000) OR genre:Drama");
    EXPECT_EQ(r.total, 2);
}

TEST_F(FilterExprIntegrationTest, AudienceRatingGte) {
    insertMovie("m1", "A", "[]", 2020, 9.0, 6000000);
    insertMovie("m2", "B", "[]", 2020, 3.0, 6000000);
    EXPECT_EQ(searchMovies("audience_rating:>=8").total, 1);
}

TEST_F(FilterExprIntegrationTest, DurationOnMoviesInMinutes) {
    insertMovie("m1", "Long",  "[]", 2020, 5, 7200000); // 120 min
    insertMovie("m2", "Short", "[]", 2020, 5, 3000000); // 50 min
    EXPECT_EQ(searchMovies("duration:>=90").total, 1);
}

TEST_F(FilterExprIntegrationTest, DecadeRange) {
    insertMovie("m1", "A", "[]", 2015, 5, 6000000);
    insertMovie("m2", "B", "[]", 1999, 5, 6000000);
    EXPECT_EQ(searchMovies("decade:2010s").total, 1);
}

TEST_F(FilterExprIntegrationTest, AddedInLastDays) {
    int64_t now = static_cast<int64_t>(time(nullptr));
    insertMovie("m1", "Recent", "[]", 2020, 5, 6000000, "", now - 3600);           // 1 hour ago
    insertMovie("m2", "Old",    "[]", 2020, 5, 6000000, "", now - 60LL * 86400);   // 60 days ago
    EXPECT_EQ(searchMovies("added:7").total, 1);
}

TEST_F(FilterExprIntegrationTest, ResolutionOnShowMatchesAnyEpisode) {
    insertShow("s1", "Mixed Res Show", "[]", 2020);
    insertEpisode("e1", "s1", "SD");
    insertEpisode("e2", "s1", "1080p");
    EXPECT_EQ(searchShows("resolution:1080p").total, 1);
    EXPECT_EQ(searchShows("resolution:4K").total, 0);
}

TEST_F(FilterExprIntegrationTest, DirectorFilterMoviesOnly) {
    insertMovie("m1", "A", "[]", 2020, 5, 6000000, "", 0, "Christopher Nolan");
    insertShow("s1", "Some Show", "[]", 2020);
    EXPECT_EQ(searchMovies("director:Nolan").total, 0); // contains not requested — exact 'is' default
    EXPECT_EQ(searchMovies("director:\"Christopher Nolan\"").total, 1);
    EXPECT_EQ(searchShows("director:Nolan").total, 0); // shows never match — 1=0 clause
}

TEST_F(FilterExprIntegrationTest, FuzzyWordMatchesAcrossFields) {
    insertMovie("m1", "The Great Movie", "[]", 2020, 5, 6000000);
    insertMovie("m2", "Something Else",  "[]", 2020, 5, 6000000);
    EXPECT_EQ(searchMovies("great").total, 1);
}

// Regression: network is show-only (movies have no such column at all) —
// searchMovies() with a network filter used to throw "no such column:
// m.network", which LibraryStore.fetch() silently swallowed via Promise.all,
// leaving stale results on screen that looked like the filter did nothing.
TEST_F(FilterExprIntegrationTest, NetworkFilterMoviesOnly) {
    insertShow("s1", "Disney Show", "[]", 2020, "Disney Channel");
    insertShow("s2", "Other Show",  "[]", 2020, "HBO");
    insertMovie("m1", "Some Movie", "[]", 2020, 5, 6000000);

    EXPECT_EQ(searchShows("network:\"Disney Channel\"").total, 1);
    EXPECT_EQ(searchShows("-network:\"Disney Channel\"").total, 1);
    // Must not throw — movies just never match a network filter.
    EXPECT_NO_THROW({ auto r = searchMovies("network:Disney"); EXPECT_EQ(r.total, 0); });
}

// Regression: the "contains" operator round-trips through the canon syntax
// as a *value* wildcard (see filterSyntax.ts's serializeClause) — this is
// what the rule builder actually sends when "contains" is picked instead of
// "is". Confirms a bare substring match (not the whole exact network name)
// actually matches, unlike the pre-fix behavior where it silently became an
// exact match and matched nothing.
TEST_F(FilterExprIntegrationTest, WildcardContainsMatchesSubstring) {
    insertShow("s1", "Disney Show", "[]", 2020, "Disney Channel");
    EXPECT_EQ(searchShows("network:*Disney*").total, 1);
    EXPECT_EQ(searchShows("network:Disney").total, 0); // exact 'is' default — no substring match
}

// New 'source' field — which media source (by source_id, not just type,
// since a user can run more than one Plex server) an item is mapped from.
// Unlike 'library', this has no separate dedicated scoping param, so it's a
// normal AND/OR/negatable canon-syntax clause.
TEST_F(FilterExprIntegrationTest, SourceFilterMatchesMappedSource) {
    insertSource("plex1", "plex");
    insertSource("jf1", "jellyfin");
    insertShow("s1", "Plex Show", "[]", 2020);
    insertShow("s2", "Jellyfin Show", "[]", 2020);
    mapToSource("show", "s1", "plex1");
    mapToSource("show", "s2", "jf1");

    EXPECT_EQ(searchShows("source:plex1").total, 1);
    EXPECT_EQ(searchShows("-source:plex1").total, 1);
    EXPECT_EQ(searchShows("source:jf1").total, 1);
}

// Seeded random sort (the Library page's die/reroll button) — same seed must
// give the same order every call (stable pagination through a "Random"
// browse), a different seed must give a (near-certainly) different order.
TEST_F(FilterExprIntegrationTest, SeededRandomSortIsStablePerSeedAndReshufflesOnNewSeed) {
    for (int i = 0; i < 20; ++i)
        insertMovie("m" + std::to_string(i), "Movie " + std::to_string(i), "[]", 2020, 5, 6000000);

    auto orderFor = [&](int64_t seed) {
        MovieSearchParams p; p.sort = "random"; p.random_seed = seed; p.limit = 20;
        auto r = repo.searchMovies(p);
        std::vector<std::string> ids;
        for (auto& m : r.items) ids.push_back(m.movie_id);
        return ids;
    };

    auto a1 = orderFor(42);
    auto a2 = orderFor(42);
    auto b  = orderFor(1337);

    EXPECT_EQ(a1, a2) << "same seed must produce the same order every call";
    EXPECT_NE(a1, b)  << "a different seed must produce a different order";
}
