#include <gtest/gtest.h>
#include "db/Database.h"
#include "scheduler/CursorState.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void insertChannel(SQLite::Database& db, const std::string& id, int number = 1) {
    SQLite::Statement s(db,
        "INSERT INTO channel (channel_id, name, number) VALUES (?,?,?)");
    s.bind(1, id); s.bind(2, "Ch " + id); s.bind(3, number);
    s.exec();
}

static void insertBlock(SQLite::Database& db,
                        const std::string& bid, const std::string& cid) {
    SQLite::Statement s(db,
        "INSERT INTO block (block_id, channel_id, block_type, start_time, day_mask)"
        " VALUES (?,?,'episode','00:00',127)");
    s.bind(1, bid); s.bind(2, cid);
    s.exec();
}

static void insertCursor(SQLite::Database& db,
                         const std::string& ct, const std::string& cid,
                         const std::string& scope, const std::string& sid,
                         int pos) {
    SQLite::Statement s(db, R"(
        INSERT INTO media_cursor
            (content_type, content_id, cursor_scope, scope_id, position, updated_at)
        VALUES (?,?,?,?,?,0)
    )");
    s.bind(1, ct); s.bind(2, cid); s.bind(3, scope); s.bind(4, sid); s.bind(5, pos);
    s.exec();
}

static void insertBlockState(SQLite::Database& db,
                              const std::string& bid, const std::string& cid,
                              int cp, int rr, int cc) {
    SQLite::Statement s(db, R"(
        INSERT INTO block_state
            (block_id, channel_id, content_position, runs_remaining, consecutive_count, updated_at)
        VALUES (?,?,?,?,?,0)
    )");
    s.bind(1, bid); s.bind(2, cid); s.bind(3, cp); s.bind(4, rr); s.bind(5, cc);
    s.exec();
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class CursorStateTest : public ::testing::Test {
protected:
    Database db{ ":memory:" };
};

// ===========================================================================
// In-memory accessors
// ===========================================================================

TEST_F(CursorStateTest, GetCursorPos_DefaultsToZero) {
    CursorState state;
    EXPECT_EQ(state.getCursorPos("show", "s1", "block", "b1"), 0);
    EXPECT_EQ(state.getCursorPos("movie", "m1", "channel", "c1"), 0);
    EXPECT_EQ(state.getCursorPos("show", "s2", "global", ""), 0);
}

TEST_F(CursorStateTest, SetAndGetCursorPos_RoundTrip) {
    CursorState state;
    state.setCursorPos("show", "s1", "block", "b1", 7);
    EXPECT_EQ(state.getCursorPos("show", "s1", "block", "b1"), 7);
}

TEST_F(CursorStateTest, SetCursorPos_WithEpisodeId) {
    CursorState state;
    state.setCursorPos("show", "s1", "block", "b1", 3, "e4");
    // The episode_id should round-trip through serialize/deserialize.
    auto json_str = state.serializeCursors();
    auto j = json::parse(json_str);
    ASSERT_FALSE(j["cursors"].empty());
    EXPECT_EQ(j["cursors"][0]["episode_id"].get<std::string>(), "e4");
}

TEST_F(CursorStateTest, SetCursorPos_MultipleKeysAreIndependent) {
    CursorState state;
    state.setCursorPos("show", "s1", "block",   "b1", 5);
    state.setCursorPos("show", "s1", "channel", "c1", 9);
    state.setCursorPos("show", "s2", "block",   "b1", 2);

    EXPECT_EQ(state.getCursorPos("show", "s1", "block",   "b1"), 5);
    EXPECT_EQ(state.getCursorPos("show", "s1", "channel", "c1"), 9);
    EXPECT_EQ(state.getCursorPos("show", "s2", "block",   "b1"), 2);
}

TEST_F(CursorStateTest, GetContentPosition_DefaultsToZero) {
    CursorState state;
    EXPECT_EQ(state.getContentPosition("b_unknown"), 0);
}

TEST_F(CursorStateTest, SetContentPosition_UpdatesPositionOnly) {
    CursorState state;
    state.setContentPosition("b1", 4);
    EXPECT_EQ(state.getContentPosition("b1"), 4);
    EXPECT_EQ(state.getRunsRemaining("b1"),    0);
    EXPECT_EQ(state.getConsecutiveCount("b1"), 0);
    EXPECT_TRUE(state.hasBlockPosition("b1"));
}

TEST_F(CursorStateTest, SetBlockPosition_AllThreeFieldsStored) {
    CursorState state;
    state.setBlockPosition("b1", 3, 2, 1);
    EXPECT_EQ(state.getContentPosition("b1"),  3);
    EXPECT_EQ(state.getRunsRemaining("b1"),    2);
    EXPECT_EQ(state.getConsecutiveCount("b1"), 1);
}

TEST_F(CursorStateTest, HasBlockPosition_FalseBeforeSet) {
    CursorState state;
    EXPECT_FALSE(state.hasBlockPosition("b_not_set"));
}

TEST_F(CursorStateTest, HasBlockPosition_TrueAfterSetContentPosition) {
    CursorState state;
    state.setContentPosition("b1", 0);
    EXPECT_TRUE(state.hasBlockPosition("b1"));
}

TEST_F(CursorStateTest, SetContentPosition_PreservesRunsRemainingAndConsecutiveCount) {
    CursorState state;
    state.setBlockPosition("b1", 1, 5, 3);
    state.setContentPosition("b1", 2);
    EXPECT_EQ(state.getContentPosition("b1"),  2);
    EXPECT_EQ(state.getRunsRemaining("b1"),    5);
    EXPECT_EQ(state.getConsecutiveCount("b1"), 3);
}

TEST_F(CursorStateTest, FillerPos_DefaultsToZero) {
    CursorState state;
    EXPECT_EQ(state.getFillerPos("fl_rr:b1"), 0);
}

TEST_F(CursorStateTest, FillerPos_RefMutatesInPlace) {
    CursorState state;
    state.fillerPos("fl_rr:b1")++;
    state.fillerPos("fl_rr:b1")++;
    EXPECT_EQ(state.getFillerPos("fl_rr:b1"), 2);
}

TEST_F(CursorStateTest, HasFillerPos_FalseBeforeAnyAccess) {
    CursorState state;
    EXPECT_FALSE(state.hasFillerPos("fl_rr:b_none"));
}

TEST_F(CursorStateTest, HasFillerPos_TrueAfterFillerPosRef) {
    CursorState state;
    (void)state.fillerPos("fl_rr:b1");  // access creates the entry
    EXPECT_TRUE(state.hasFillerPos("fl_rr:b1"));
}

TEST_F(CursorStateTest, PlayRecords_InitiallyEmpty) {
    CursorState state;
    EXPECT_TRUE(state.playRecords().empty());
}

TEST_F(CursorStateTest, AddPlayRecord_Accumulates) {
    CursorState state;
    state.addPlayRecord("c1", "episode", "e1", "", "", 1000);
    state.addPlayRecord("c1", "episode", "e2", "", "", 2000);
    state.addPlayRecord("c1", "movie",   "m1", "", "", 3000);
    ASSERT_EQ(state.playRecords().size(), 3u);
    EXPECT_EQ(state.playRecords()[0].item_type,  "episode");
    EXPECT_EQ(state.playRecords()[0].item_id,    "e1");
    EXPECT_EQ(state.playRecords()[2].item_type,  "movie");
    EXPECT_EQ(state.playRecords()[2].aired_at,   3000);
}

// ===========================================================================
// DB I/O
// ===========================================================================

TEST_F(CursorStateTest, LoadFromDB_EmptyChannelProducesEmptyState) {
    insertChannel(db.get(), "c1");
    auto state = CursorState::loadFromDB(db, "c1");
    EXPECT_EQ(state.getCursorPos("show", "s1", "channel", "c1"), 0);
    EXPECT_FALSE(state.hasBlockPosition("b1"));
}

TEST_F(CursorStateTest, LoadFromDB_LoadsChannelScopedCursor) {
    insertChannel(db.get(), "c1");
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Test')");
    insertCursor(db.get(), "show", "s1", "channel", "c1", 7);

    auto state = CursorState::loadFromDB(db, "c1");
    EXPECT_EQ(state.getCursorPos("show", "s1", "channel", "c1"), 7);
}

TEST_F(CursorStateTest, LoadFromDB_LoadsBlockScopedCursor) {
    insertChannel(db.get(), "c1");
    insertBlock(db.get(), "b1", "c1");
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Test')");
    insertCursor(db.get(), "show", "s1", "block", "b1", 3);

    auto state = CursorState::loadFromDB(db, "c1");
    EXPECT_EQ(state.getCursorPos("show", "s1", "block", "b1"), 3);
}

TEST_F(CursorStateTest, LoadFromDB_LoadsGlobalCursorForChannelContent) {
    insertChannel(db.get(), "c1");
    insertBlock(db.get(), "b1", "c1");
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Test')");
    db.get().exec(
        "INSERT INTO block_content (block_id, content_type, content_id)"
        " VALUES ('b1','show','s1')");
    insertCursor(db.get(), "show", "s1", "global", "", 5);

    auto state = CursorState::loadFromDB(db, "c1");
    EXPECT_EQ(state.getCursorPos("show", "s1", "global", ""), 5);
}

TEST_F(CursorStateTest, LoadFromDB_DoesNotLoadGlobalCursorForUnrelatedContent) {
    insertChannel(db.get(), "c1");
    insertBlock(db.get(), "b1", "c1");
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Linked')");
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s2','Unrelated')");
    db.get().exec(
        "INSERT INTO block_content (block_id, content_type, content_id)"
        " VALUES ('b1','show','s1')");
    insertCursor(db.get(), "show", "s2", "global", "", 9);

    auto state = CursorState::loadFromDB(db, "c1");
    EXPECT_EQ(state.getCursorPos("show", "s2", "global", ""), 0);
}

TEST_F(CursorStateTest, LoadFromDB_LoadsBlockState) {
    insertChannel(db.get(), "c1");
    insertBlock(db.get(), "b1", "c1");
    insertBlockState(db.get(), "b1", "c1", 2, 4, 1);

    auto state = CursorState::loadFromDB(db, "c1");
    EXPECT_EQ(state.getContentPosition("b1"),  2);
    EXPECT_EQ(state.getRunsRemaining("b1"),    4);
    EXPECT_EQ(state.getConsecutiveCount("b1"), 1);
}

TEST_F(CursorStateTest, ApplyToDB_PersistsChannelCursor) {
    insertChannel(db.get(), "c1");
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Test')");
    CursorState state;
    state.setCursorPos("show", "s1", "channel", "c1", 5);
    state.applyToDB(db, "c1");

    SQLite::Statement q(db.get(),
        "SELECT position FROM media_cursor"
        " WHERE content_type='show' AND content_id='s1'"
        "   AND cursor_scope='channel' AND scope_id='c1'");
    ASSERT_TRUE(q.executeStep());
    EXPECT_EQ(q.getColumn(0).getInt(), 5);
}

TEST_F(CursorStateTest, ApplyToDB_PersistsBlockState) {
    insertChannel(db.get(), "c1");
    insertBlock(db.get(), "b1", "c1");
    CursorState state;
    state.setBlockPosition("b1", 3, 2, 1);
    state.applyToDB(db, "c1");

    SQLite::Statement q(db.get(),
        "SELECT content_position, runs_remaining, consecutive_count"
        " FROM block_state WHERE block_id='b1' AND channel_id='c1'");
    ASSERT_TRUE(q.executeStep());
    EXPECT_EQ(q.getColumn(0).getInt(), 3);
    EXPECT_EQ(q.getColumn(1).getInt(), 2);
    EXPECT_EQ(q.getColumn(2).getInt(), 1);
}

TEST_F(CursorStateTest, ApplyToDB_DeletesStaleChannelCursors) {
    insertChannel(db.get(), "c1");
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Test')");
    insertCursor(db.get(), "show", "s1", "channel", "c1", 9);

    auto state = CursorState::loadFromDB(db, "c1");
    state.setCursorPos("show", "s1", "channel", "c1", 2);
    state.applyToDB(db, "c1");

    SQLite::Statement q(db.get(),
        "SELECT COUNT(*), position FROM media_cursor"
        " WHERE cursor_scope='channel' AND scope_id='c1'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 1) << "Old row must be replaced, not duplicated";
    EXPECT_EQ(q.getColumn(1).getInt(), 2);
}

TEST_F(CursorStateTest, ApplyToDB_GlobalCursorsAreUpserted) {
    insertChannel(db.get(), "c1");
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Test')");

    CursorState state;
    state.setCursorPos("show", "s1", "global", "", 5);
    state.applyToDB(db, "c1");

    {
        SQLite::Statement q(db.get(),
            "SELECT COUNT(*) FROM media_cursor WHERE cursor_scope='global'");
        q.executeStep();
        EXPECT_EQ(q.getColumn(0).getInt(), 1);
    }

    state.setCursorPos("show", "s1", "global", "", 9);
    state.applyToDB(db, "c1");

    SQLite::Statement q(db.get(),
        "SELECT COUNT(*), position FROM media_cursor WHERE cursor_scope='global'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 1) << "Second applyToDB must not create a duplicate row";
    EXPECT_EQ(q.getColumn(1).getInt(), 9);
}

TEST_F(CursorStateTest, ClearFromDB_RemovesChannelAndBlockScopedState) {
    insertChannel(db.get(), "c1");
    insertBlock(db.get(), "b1", "c1");
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Test')");
    insertCursor(db.get(), "show", "s1", "channel", "c1", 3);
    insertBlockState(db.get(), "b1", "c1", 1, 0, 0);

    CursorState::clearFromDB(db, "c1");

    SQLite::Statement qc(db.get(),
        "SELECT COUNT(*) FROM media_cursor WHERE cursor_scope='channel' AND scope_id='c1'");
    qc.executeStep();
    EXPECT_EQ(qc.getColumn(0).getInt(), 0);

    SQLite::Statement qb(db.get(),
        "SELECT COUNT(*) FROM block_state WHERE channel_id='c1'");
    qb.executeStep();
    EXPECT_EQ(qb.getColumn(0).getInt(), 0);
}

TEST_F(CursorStateTest, ClearFromDB_PreservesGlobalScopedCursors) {
    insertChannel(db.get(), "c1");
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Test')");
    insertCursor(db.get(), "show", "s1", "global",  "",   5);
    insertCursor(db.get(), "show", "s1", "channel", "c1", 3);

    CursorState::clearFromDB(db, "c1");

    SQLite::Statement q(db.get(),
        "SELECT COUNT(*) FROM media_cursor WHERE cursor_scope='global'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 1) << "Global cursor must survive clearFromDB";
}

TEST_F(CursorStateTest, ClearFromDB_PreservesOtherChannelsState) {
    insertChannel(db.get(), "c1");
    insertChannel(db.get(), "c2", 2);
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Test')");
    insertCursor(db.get(), "show", "s1", "channel", "c1", 3);
    insertCursor(db.get(), "show", "s1", "channel", "c2", 7);

    CursorState::clearFromDB(db, "c1");

    SQLite::Statement q(db.get(),
        "SELECT position FROM media_cursor"
        " WHERE cursor_scope='channel' AND scope_id='c2'");
    ASSERT_TRUE(q.executeStep()) << "c2 cursor must still exist after clearing c1";
    EXPECT_EQ(q.getColumn(0).getInt(), 7);
}

// ===========================================================================
// Anchor serialization
// ===========================================================================

TEST_F(CursorStateTest, SerializeCursors_ProducesValidJson) {
    CursorState state;
    state.setCursorPos("show", "s1", "block", "b1", 4);
    state.setBlockPosition("b1", 1, 3, 0);

    EXPECT_NO_THROW({
        auto s = state.serializeCursors();
        auto j = json::parse(s);
        (void)j;
    });
}

TEST_F(CursorStateTest, SerializeCursors_ContainsCursorsAndBlockStatesKeys) {
    CursorState state;
    state.setCursorPos("show", "s1", "block", "b1", 2);
    state.setBlockPosition("b1", 0, 1, 0);

    auto j = json::parse(state.serializeCursors());
    EXPECT_TRUE(j.contains("cursors"))      << "JSON must have 'cursors' key";
    EXPECT_TRUE(j.contains("block_states")) << "JSON must have 'block_states' key";
    EXPECT_TRUE(j["cursors"].is_array());
    EXPECT_TRUE(j["block_states"].is_array());
}

TEST_F(CursorStateTest, DeserializeCursors_EmptyJsonProducesEmptyState) {
    auto state = CursorState::deserializeCursors("{}");
    EXPECT_EQ(state.getCursorPos("show", "s1", "block", "b1"), 0);
    EXPECT_FALSE(state.hasBlockPosition("b1"));
}

TEST_F(CursorStateTest, DeserializeCursors_MalformedInputDoesNotCrash) {
    EXPECT_NO_THROW({
        auto state = CursorState::deserializeCursors("not json at all");
        (void)state;
    });
}

TEST_F(CursorStateTest, DeserializeCursors_RestoresCursorPosition) {
    const std::string j = R"({
        "cursors": [{
            "content_type": "show",
            "content_id":   "s1",
            "cursor_scope": "block",
            "scope_id":     "b1",
            "position":     7,
            "episode_id":   ""
        }],
        "block_states": []
    })";
    auto state = CursorState::deserializeCursors(j);
    EXPECT_EQ(state.getCursorPos("show", "s1", "block", "b1"), 7);
}

TEST_F(CursorStateTest, DeserializeCursors_RestoresBlockState) {
    const std::string j = R"({
        "cursors": [],
        "block_states": [{
            "block_id":          "b1",
            "content_position":  3,
            "runs_remaining":    2,
            "consecutive_count": 1
        }]
    })";
    auto state = CursorState::deserializeCursors(j);
    EXPECT_EQ(state.getContentPosition("b1"),  3);
    EXPECT_EQ(state.getRunsRemaining("b1"),    2);
    EXPECT_EQ(state.getConsecutiveCount("b1"), 1);
}

TEST_F(CursorStateTest, RoundTrip_SerializeDeserializePreservesAllState) {
    CursorState orig;
    orig.setCursorPos("show",  "s1", "block",   "b1", 4, "e5");
    orig.setCursorPos("show",  "s2", "channel", "c1", 2, "");
    orig.setCursorPos("movie", "m1", "global",  "",   1, "");
    orig.setBlockPosition("b1", 3, 2, 1);
    orig.setBlockPosition("b2", 0, 5, 0);
    // filler_positions_ are transient and intentionally excluded from serialization.

    auto state = CursorState::deserializeCursors(orig.serializeCursors());

    EXPECT_EQ(state.getCursorPos("show",  "s1", "block",   "b1"), 4);
    EXPECT_EQ(state.getCursorPos("show",  "s2", "channel", "c1"), 2);
    EXPECT_EQ(state.getCursorPos("movie", "m1", "global",  ""),   1);
    EXPECT_EQ(state.getContentPosition("b1"),  3);
    EXPECT_EQ(state.getRunsRemaining("b1"),    2);
    EXPECT_EQ(state.getConsecutiveCount("b1"), 1);
    EXPECT_EQ(state.getContentPosition("b2"),  0);
    EXPECT_EQ(state.getRunsRemaining("b2"),    5);
}
