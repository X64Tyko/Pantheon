#include <gtest/gtest.h>
#include "db/SourceRepository.h"
#include "db/Database.h"
#include "model/SourceConfig.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <string>
#include <vector>

class SourceRepositoryTest : public ::testing::Test {
protected:
    Database         db{ ":memory:" };
    SourceRepository repo{ db };

    void insertSource(const std::string& source_id) {
        SQLite::Statement s(db.get(),
            "INSERT INTO media_source (source_id, source_type, display_name, base_url) VALUES (?,?,?,?)");
        s.bind(1, source_id); s.bind(2, "jellyfin"); s.bind(3, source_id + " display"); s.bind(4, "http://x");
        s.exec();
    }

    void insertUserRow(const std::string& user_id) {
        SQLite::Statement s(db.get(),
            "INSERT INTO user (user_id, username, password_hash, role, created_at) VALUES (?,?,?,?,0)");
        s.bind(1, user_id); s.bind(2, user_id + "_username"); s.bind(3, "hash"); s.bind(4, "viewer");
        s.exec();
    }
};

// ---------------------------------------------------------------------------
// upsertSourceUsers / listUnmappedSourceUsers
// ---------------------------------------------------------------------------

TEST_F(SourceRepositoryTest, UpsertSourceUsers_InsertsNewRows) {
    insertSource("src1");
    std::vector<SourceUserInfo> users = {
        {"ext1", "Alice", "alice@example.com"},
        {"ext2", "Bob", ""},
    };
    repo.upsertSourceUsers("src1", users, 1000);

    EXPECT_EQ(repo.listUnmappedSourceUsers().size(), 2u);
}

TEST_F(SourceRepositoryTest, UpsertSourceUsers_UpdatesExistingRowFields) {
    insertSource("src1");
    repo.upsertSourceUsers("src1", {{"ext1", "Alice", ""}}, 1000);
    repo.upsertSourceUsers("src1", {{"ext1", "Alice Renamed", "alice@example.com"}}, 2000);

    auto unmapped = repo.listUnmappedSourceUsers();
    ASSERT_EQ(unmapped.size(), 1u);
    EXPECT_EQ(unmapped[0].display_name, "Alice Renamed");
    EXPECT_EQ(unmapped[0].email, "alice@example.com");
}

TEST_F(SourceRepositoryTest, UpsertSourceUsers_PreservesImportedUserIdOnConflict) {
    insertSource("src1");
    insertUserRow("u1");
    repo.upsertSourceUsers("src1", {{"ext1", "Alice", ""}}, 1000);
    repo.setImportedUserId("src1", "ext1", "u1");

    // Re-discovering the same account on a later sync must not un-import it.
    repo.upsertSourceUsers("src1", {{"ext1", "Alice", "alice@example.com"}}, 2000);

    EXPECT_TRUE(repo.listUnmappedSourceUsers().empty());
}

TEST_F(SourceRepositoryTest, SetImportedUserId_ExcludesFromUnmappedList) {
    insertSource("src1");
    insertUserRow("u1");
    repo.upsertSourceUsers("src1", {{"ext1", "Alice", ""}, {"ext2", "Bob", ""}}, 1000);
    ASSERT_EQ(repo.listUnmappedSourceUsers().size(), 2u);

    repo.setImportedUserId("src1", "ext1", "u1");

    auto unmapped = repo.listUnmappedSourceUsers();
    ASSERT_EQ(unmapped.size(), 1u);
    EXPECT_EQ(unmapped[0].external_user_id, "ext2");
}

TEST_F(SourceRepositoryTest, ListUnmappedSourceUsers_SpansMultipleSources) {
    insertSource("src1");
    insertSource("src2");
    repo.upsertSourceUsers("src1", {{"ext1", "Alice", ""}}, 1000);
    repo.upsertSourceUsers("src2", {{"ext1", "Carol", ""}}, 1000);

    EXPECT_EQ(repo.listUnmappedSourceUsers().size(), 2u);
}

TEST_F(SourceRepositoryTest, ListUnmappedSourceUsers_EmptyWhenNoneDiscovered) {
    insertSource("src1");
    EXPECT_TRUE(repo.listUnmappedSourceUsers().empty());
}

// ---------------------------------------------------------------------------
// getSourceUser
// ---------------------------------------------------------------------------

TEST_F(SourceRepositoryTest, GetSourceUser_ReturnsRowOrNullopt) {
    insertSource("src1");
    repo.upsertSourceUsers("src1", {{"ext1", "Alice", "alice@example.com"}}, 1000);

    auto found = repo.getSourceUser("src1", "ext1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->display_name, "Alice");
    EXPECT_EQ(found->email, "alice@example.com");

    EXPECT_FALSE(repo.getSourceUser("src1", "no-such-user").has_value());
}

// ---------------------------------------------------------------------------
// synced_user_id (watch-state sync target)
// ---------------------------------------------------------------------------

TEST_F(SourceRepositoryTest, SyncedUserId_DefaultsEmptyThenRoundTrips) {
    insertSource("src1");
    insertUserRow("u1");
    EXPECT_EQ(repo.getSyncedUserId("src1"), "");

    repo.setSyncedUserId("src1", "u1");
    EXPECT_EQ(repo.getSyncedUserId("src1"), "u1");

    repo.setSyncedUserId("src1", "");
    EXPECT_EQ(repo.getSyncedUserId("src1"), "");
}

// ---------------------------------------------------------------------------
// Writeback settings — auto_writeback defaults OFF, the three per-field
// toggles default ON (see Database.cpp v85's migration comment for why).
// ---------------------------------------------------------------------------

TEST_F(SourceRepositoryTest, WritebackSettings_DefaultsMatchMigrationIntent) {
    insertSource("src1");
    auto s = repo.getSource("src1");
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s->auto_writeback);
    EXPECT_TRUE(s->writeback_update_art);
    EXPECT_TRUE(s->writeback_update_external_ids);
    EXPECT_TRUE(s->writeback_update_collections);
}

TEST_F(SourceRepositoryTest, WritebackSettings_SettersRoundTripViaGetSource) {
    insertSource("src1");

    repo.setAutoWriteback("src1", true);
    repo.setWritebackUpdateArt("src1", false);
    repo.setWritebackUpdateExternalIds("src1", false);
    repo.setWritebackUpdateCollections("src1", false);

    auto s = repo.getSource("src1");
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->auto_writeback);
    EXPECT_FALSE(s->writeback_update_art);
    EXPECT_FALSE(s->writeback_update_external_ids);
    EXPECT_FALSE(s->writeback_update_collections);
}

TEST_F(SourceRepositoryTest, WritebackSettings_ListSourcesReflectsSameValues) {
    insertSource("src1");
    repo.setAutoWriteback("src1", true);
    repo.setWritebackUpdateArt("src1", false);

    auto sources = repo.listSources();
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_TRUE(sources[0].auto_writeback);
    EXPECT_FALSE(sources[0].writeback_update_art);
    EXPECT_TRUE(sources[0].writeback_update_external_ids);  // untouched, still default
}

TEST_F(SourceRepositoryTest, WritebackSettings_CarriedOnWritebackTarget) {
    insertSource("src1");
    repo.setAutoWriteback("src1", true);
    repo.setWritebackUpdateCollections("src1", false);

    { SQLite::Statement s(db.get(),
        "INSERT INTO source_mapping (item_type, kairos_id, source_id, external_id) VALUES ('show','show1','src1','ext1')");
      s.exec(); }

    auto targets = repo.getWritebackTargets("show", "show1");
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_TRUE(targets[0].auto_writeback);
    EXPECT_TRUE(targets[0].writeback_update_art);
    EXPECT_TRUE(targets[0].writeback_update_external_ids);
    EXPECT_FALSE(targets[0].writeback_update_collections);
}
