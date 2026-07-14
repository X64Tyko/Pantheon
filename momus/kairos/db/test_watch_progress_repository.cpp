#include <gtest/gtest.h>
#include "db/WatchProgressRepository.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <string>

class WatchProgressRepositoryTest : public ::testing::Test {
protected:
    Database                db{ ":memory:" };
    WatchProgressRepository repo{ db };

    void insertUserRow(const std::string& user_id) {
        SQLite::Statement s(db.get(),
            "INSERT INTO user (user_id, username, password_hash, role, created_at) VALUES (?,?,?,?,0)");
        s.bind(1, user_id); s.bind(2, user_id + "_username"); s.bind(3, "hash"); s.bind(4, "viewer");
        s.exec();
    }

    int64_t completedFor(const std::string& user_id, const std::string& content_type, const std::string& content_id) {
        SQLite::Statement q(db.get(),
            "SELECT completed FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");
        q.bind(1, user_id); q.bind(2, content_type); q.bind(3, content_id);
        return q.executeStep() ? q.getColumn(0).getInt64() : -1;
    }

    void setUp() { insertUserRow("u1"); }
};

// ---------------------------------------------------------------------------
// upsert — completed is the rewatch count, not a 0/1 flag
// ---------------------------------------------------------------------------

TEST_F(WatchProgressRepositoryTest, Upsert_FirstCompletion_SetsCountToOne) {
    insertUserRow("u1");
    repo.upsert("u1", "movie", "m1", /*position*/ 6000000, /*duration*/ 6000000, /*updated_at*/ 1000, /*completed*/ true);

    EXPECT_EQ(completedFor("u1", "movie", "m1"), 1);
}

TEST_F(WatchProgressRepositoryTest, Upsert_NeverCompleted_CountStaysZero) {
    insertUserRow("u1");
    repo.upsert("u1", "movie", "m1", /*position*/ 1000000, /*duration*/ 6000000, /*updated_at*/ 1000, /*completed*/ false);
    repo.upsert("u1", "movie", "m1", /*position*/ 3000000, /*duration*/ 6000000, /*updated_at*/ 2000, /*completed*/ false);

    EXPECT_EQ(completedFor("u1", "movie", "m1"), 0);
}

TEST_F(WatchProgressRepositoryTest, Upsert_RepeatedPingsPastThreshold_DoNotDoubleCount) {
    insertUserRow("u1");
    // A player pings every few seconds while at/over the finish line — each
    // of these must not be treated as a fresh completion.
    repo.upsert("u1", "movie", "m1", 6000000, 6000000, 1000, true);
    repo.upsert("u1", "movie", "m1", 6000000, 6000000, 1010, true);
    repo.upsert("u1", "movie", "m1", 6000000, 6000000, 1020, true);

    EXPECT_EQ(completedFor("u1", "movie", "m1"), 1);
}

TEST_F(WatchProgressRepositoryTest, Upsert_RewatchAfterCompletion_IncrementsOnSecondCompletion) {
    insertUserRow("u1");
    repo.upsert("u1", "movie", "m1", 6000000, 6000000, 1000, true);  // first watch finishes
    repo.upsert("u1", "movie", "m1", 500000,  6000000, 2000, false); // rewatch starts, well below threshold
    EXPECT_EQ(completedFor("u1", "movie", "m1"), 1); // count preserved while rewatch is in progress

    repo.upsert("u1", "movie", "m1", 6000000, 6000000, 3000, true);  // rewatch finishes
    EXPECT_EQ(completedFor("u1", "movie", "m1"), 2);
}

TEST_F(WatchProgressRepositoryTest, Upsert_PositionAtExactDuration_CountsAsFinished) {
    insertUserRow("u1");
    // PlaybackService clamps position to duration on finish — the increment
    // condition's "already at a finished position" check must treat
    // position == duration as finished, not just position > duration.
    repo.upsert("u1", "movie", "m1", 6000000, 6000000, 1000, true);
    repo.upsert("u1", "movie", "m1", 6000000, 6000000, 2000, true);

    EXPECT_EQ(completedFor("u1", "movie", "m1"), 1);
}
