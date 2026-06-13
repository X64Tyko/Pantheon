#include <gtest/gtest.h>
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Fixture with INSERT helpers
// ---------------------------------------------------------------------------

class SchemaConstraintTest : public ::testing::Test {
protected:
    Database db{ ":memory:" };

    void insertChannel(const std::string& id, int number,
                       const std::string& name = "Channel") {
        SQLite::Statement s(db.get(),
            "INSERT INTO channel (channel_id, name, number) VALUES (?,?,?)");
        s.bind(1, id); s.bind(2, name); s.bind(3, number);
        s.exec();
    }

    void insertShow(const std::string& id, const std::string& title = "Show") {
        SQLite::Statement s(db.get(),
            "INSERT INTO show (show_id, title) VALUES (?,?)");
        s.bind(1, id); s.bind(2, title);
        s.exec();
    }

    void insertBlock(const std::string& blockId, const std::string& chanId,
                     const std::string& type = "episode") {
        SQLite::Statement s(db.get(),
            "INSERT INTO block (block_id, channel_id, block_type) VALUES (?,?,?)");
        s.bind(1, blockId); s.bind(2, chanId); s.bind(3, type);
        s.exec();
    }

    void insertSource(const std::string& id,
                      const std::string& type = "plex") {
        SQLite::Statement s(db.get(),
            "INSERT INTO media_source (source_id, source_type, display_name, base_url)"
            " VALUES (?,?,?,?)");
        s.bind(1, id); s.bind(2, type);
        s.bind(3, "Test Source"); s.bind(4, "http://localhost");
        s.exec();
    }
};

// ---------------------------------------------------------------------------
// Foreign key constraints
// ---------------------------------------------------------------------------

TEST_F(SchemaConstraintTest, FK_EpisodeRequiresExistingShow) {
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO episode"
            " (episode_id, show_id, season, episode, title, file_path, duration_ms)"
            " VALUES ('e1','nosuchshow',1,1,'Pilot','/path/ep.mkv',3600000)");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, FK_BlockRequiresExistingChannel) {
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO block (block_id, channel_id, block_type)"
            " VALUES ('b1','bogus_chan','episode')");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, FK_BlockContentRequiresExistingBlock) {
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO block_content (block_id, content_type, content_id)"
            " VALUES ('noblock','show','s1')");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, FK_MediaLibraryRequiresExistingSource) {
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO media_library"
            " (library_id, source_id, external_lib_id, display_name, library_type)"
            " VALUES ('lib1','nosource','ext1','Movies','movie')");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, FK_EpisodeInsertsSucceedWithValidShow) {
    insertShow("s1");
    EXPECT_NO_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO episode"
            " (episode_id, show_id, season, episode, title, file_path, duration_ms)"
            " VALUES ('e1','s1',1,1,'Pilot','/path/ep.mkv',3600000)");
        s.exec();
    });
}

// ---------------------------------------------------------------------------
// CHECK constraints — block_type enum
// ---------------------------------------------------------------------------

TEST_F(SchemaConstraintTest, Check_BlockTypeRejectsUnknownValue) {
    insertChannel("c1", 1);
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO block (block_id, channel_id, block_type)"
            " VALUES ('b1','c1','unknown')");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, Check_BlockTypeAcceptsAllFourValidValues) {
    insertChannel("c1", 1);
    for (const auto& [bid, type] : std::vector<std::pair<std::string,std::string>>{
        {"b1","episode"}, {"b2","premier"}, {"b3","filler"}, {"b4","movie"}
    }) {
        EXPECT_NO_THROW({ insertBlock(bid, "c1", type); })
            << "Rejected valid block_type: " << type;
    }
}

// ---------------------------------------------------------------------------
// CHECK constraints — advancement enum
// ---------------------------------------------------------------------------

TEST_F(SchemaConstraintTest, Check_BlockAdvancementRejectsInvalid) {
    insertChannel("c1", 1);
    insertBlock("b1", "c1");
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "UPDATE block SET advancement='random' WHERE block_id='b1'");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, Check_FillerListAdvancementRejectsInvalid) {
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO filler_list (filler_list_id, title, advancement)"
            " VALUES ('fl1','Filler','weekly')");
        s.exec();
    }, std::exception);
}

// ---------------------------------------------------------------------------
// CHECK constraints — media_source source_type
// ---------------------------------------------------------------------------

TEST_F(SchemaConstraintTest, Check_MediaSourceTypeRejectsUnknown) {
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO media_source (source_id, source_type, display_name)"
            " VALUES ('s1','hdhr','HDHomeRun')");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, Check_MediaSourceTypeAcceptsAllFourValid) {
    for (const auto& [sid, type] : std::vector<std::pair<std::string,std::string>>{
        {"s1","plex"}, {"s2","jellyfin"}, {"s3","emby"}, {"s4","local"}
    }) {
        EXPECT_NO_THROW({ insertSource(sid, type); })
            << "Rejected valid source_type: " << type;
    }
}

// ---------------------------------------------------------------------------
// CHECK constraints — scheduled_program status
// ---------------------------------------------------------------------------

TEST_F(SchemaConstraintTest, Check_ScheduledProgramStatusRejectsInvalid) {
    insertChannel("c1", 1);
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO scheduled_program"
            " (channel_id, item_type, item_id, wall_clock_start, wall_clock_end,"
            "  status, cursor_json, created_at)"
            " VALUES ('c1','episode','e1',0,3600000,'pending','{}',0)");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, Check_ScheduledProgramAcceptsValidStatuses) {
    insertChannel("c1", 1);
    int start = 0;
    for (const auto& [eid, status] : std::vector<std::pair<std::string,std::string>>{
        {"e1","scheduled"}, {"e2","aired"}, {"e3","skipped"}
    }) {
        int end = start + 3600000;
        EXPECT_NO_THROW({
            SQLite::Statement s(db.get(),
                "INSERT INTO scheduled_program"
                " (channel_id, item_type, item_id, wall_clock_start, wall_clock_end,"
                "  status, cursor_json, created_at)"
                " VALUES ('c1','episode',?,?,?,'"+status+"','{}',0)");
            s.bind(1, eid); s.bind(2, start); s.bind(3, end);
            s.exec();
        }) << "Rejected valid status: " << status;
        start = end;
    }
}

// ---------------------------------------------------------------------------
// UNIQUE constraints
// ---------------------------------------------------------------------------

TEST_F(SchemaConstraintTest, Unique_ChannelNumberIsUnique) {
    insertChannel("c1", 42);
    EXPECT_THROW({ insertChannel("c2", 42); }, std::exception);
}

TEST_F(SchemaConstraintTest, Unique_BlockContentNoDuplicates) {
    insertChannel("c1", 1);
    insertBlock("b1", "c1");
    insertShow("s1");
    {
        SQLite::Statement s(db.get(),
            "INSERT INTO block_content (block_id, content_type, content_id)"
            " VALUES ('b1','show','s1')");
        s.exec();
    }
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO block_content (block_id, content_type, content_id)"
            " VALUES ('b1','show','s1')");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, Unique_BlockFillerEntryNoDuplicates) {
    insertChannel("c1", 1);
    insertBlock("b1", "c1");
    SQLite::Statement fl(db.get(),
        "INSERT INTO filler_list (filler_list_id, title) VALUES ('fl1','Ads')");
    fl.exec();
    {
        SQLite::Statement s(db.get(),
            "INSERT INTO block_filler_entry (block_id, filler_list_id, advancement)"
            " VALUES ('b1','fl1','sequential')");
        s.exec();
    }
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO block_filler_entry (block_id, filler_list_id, advancement)"
            " VALUES ('b1','fl1','shuffle')");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, Unique_MediaLibrarySourceExternalId) {
    insertSource("src1");
    {
        SQLite::Statement s(db.get(),
            "INSERT INTO media_library"
            " (library_id, source_id, external_lib_id, display_name, library_type)"
            " VALUES ('lib1','src1','ext1','Movies','movie')");
        s.exec();
    }
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO media_library"
            " (library_id, source_id, external_lib_id, display_name, library_type)"
            " VALUES ('lib2','src1','ext1','Movies-Dupe','movie')");
        s.exec();
    }, std::exception);
}

TEST_F(SchemaConstraintTest, Unique_ScheduledProgramChannelStartTime) {
    insertChannel("c1", 1);
    {
        SQLite::Statement s(db.get(),
            "INSERT INTO scheduled_program"
            " (channel_id, item_type, item_id, wall_clock_start, wall_clock_end,"
            "  status, cursor_json, created_at)"
            " VALUES ('c1','episode','e1',0,3600000,'scheduled','{}',0)");
        s.exec();
    }
    EXPECT_THROW({
        SQLite::Statement s(db.get(),
            "INSERT INTO scheduled_program"
            " (channel_id, item_type, item_id, wall_clock_start, wall_clock_end,"
            "  status, cursor_json, created_at)"
            " VALUES ('c1','episode','e2',0,7200000,'scheduled','{}',0)");
        s.exec();
    }, std::exception) << "Same channel + wall_clock_start must be unique";
}

// ---------------------------------------------------------------------------
// CASCADE deletes
// ---------------------------------------------------------------------------

TEST_F(SchemaConstraintTest, Cascade_DeleteBlockRemovesBlockContent) {
    insertChannel("c1", 1);
    insertBlock("b1", "c1");
    insertShow("s1");
    {
        SQLite::Statement s(db.get(),
            "INSERT INTO block_content (block_id, content_type, content_id)"
            " VALUES ('b1','show','s1')");
        s.exec();
    }
    SQLite::Statement del(db.get(), "DELETE FROM block WHERE block_id='b1'");
    del.exec();
    SQLite::Statement check(db.get(),
        "SELECT COUNT(*) FROM block_content WHERE block_id='b1'");
    check.executeStep();
    EXPECT_EQ(check.getColumn(0).getInt(), 0);
}

TEST_F(SchemaConstraintTest, Cascade_DeleteChannelRemovesBlocks) {
    insertChannel("c1", 1);
    insertBlock("b1", "c1");
    insertBlock("b2", "c1");
    SQLite::Statement del(db.get(), "DELETE FROM channel WHERE channel_id='c1'");
    del.exec();
    SQLite::Statement check(db.get(),
        "SELECT COUNT(*) FROM block WHERE channel_id='c1'");
    check.executeStep();
    EXPECT_EQ(check.getColumn(0).getInt(), 0);
}

TEST_F(SchemaConstraintTest, Cascade_DeletePlaylistRemovesItems) {
    SQLite::Statement pl(db.get(),
        "INSERT INTO playlist (playlist_id, title) VALUES ('p1','My Playlist')");
    pl.exec();
    SQLite::Statement item(db.get(),
        "INSERT INTO playlist_item (playlist_id, position, item_type, item_id)"
        " VALUES ('p1',0,'movie','m1')");
    item.exec();
    SQLite::Statement del(db.get(),
        "DELETE FROM playlist WHERE playlist_id='p1'");
    del.exec();
    SQLite::Statement check(db.get(),
        "SELECT COUNT(*) FROM playlist_item WHERE playlist_id='p1'");
    check.executeStep();
    EXPECT_EQ(check.getColumn(0).getInt(), 0);
}

TEST_F(SchemaConstraintTest, Cascade_DeleteFillerListRemovesItems) {
    SQLite::Statement fl(db.get(),
        "INSERT INTO filler_list (filler_list_id, title) VALUES ('fl1','Bumpers')");
    fl.exec();
    SQLite::Statement item(db.get(),
        "INSERT INTO filler_list_item (filler_list_id, item_type, item_id)"
        " VALUES ('fl1','episode','e1')");
    item.exec();
    SQLite::Statement del(db.get(),
        "DELETE FROM filler_list WHERE filler_list_id='fl1'");
    del.exec();
    SQLite::Statement check(db.get(),
        "SELECT COUNT(*) FROM filler_list_item WHERE filler_list_id='fl1'");
    check.executeStep();
    EXPECT_EQ(check.getColumn(0).getInt(), 0);
}

// ---------------------------------------------------------------------------
// Default values
// ---------------------------------------------------------------------------

TEST_F(SchemaConstraintTest, Default_ChannelTimezoneIsUTC) {
    insertChannel("c1", 1);
    SQLite::Statement q(db.get(),
        "SELECT timezone FROM channel WHERE channel_id='c1'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getString(), "UTC");
}

TEST_F(SchemaConstraintTest, Default_ChannelSeedIs12345) {
    insertChannel("c1", 1);
    SQLite::Statement q(db.get(),
        "SELECT seed FROM channel WHERE channel_id='c1'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 12345);
}

TEST_F(SchemaConstraintTest, Default_BlockDayMaskIs127AllDays) {
    insertChannel("c1", 1);
    insertBlock("b1", "c1");
    SQLite::Statement q(db.get(),
        "SELECT day_mask FROM block WHERE block_id='b1'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 127);
}

TEST_F(SchemaConstraintTest, Default_BlockPriorityIsZero) {
    insertChannel("c1", 1);
    insertBlock("b1", "c1");
    SQLite::Statement q(db.get(),
        "SELECT priority FROM block WHERE block_id='b1'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 0);
}

TEST_F(SchemaConstraintTest, Default_FillerListAdvancementIsShuffle) {
    SQLite::Statement s(db.get(),
        "INSERT INTO filler_list (filler_list_id, title) VALUES ('fl1','Fillers')");
    s.exec();
    SQLite::Statement q(db.get(),
        "SELECT advancement FROM filler_list WHERE filler_list_id='fl1'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getString(), "shuffle");
}

TEST_F(SchemaConstraintTest, Default_ScheduledProgramStatusIsScheduled) {
    insertChannel("c1", 1);
    SQLite::Statement s(db.get(),
        "INSERT INTO scheduled_program"
        " (channel_id, item_type, item_id, wall_clock_start, wall_clock_end,"
        "  cursor_json, created_at)"
        " VALUES ('c1','episode','e1',0,3600000,'{}',0)");
    s.exec();
    SQLite::Statement q(db.get(),
        "SELECT status FROM scheduled_program WHERE channel_id='c1'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getString(), "scheduled");
}
