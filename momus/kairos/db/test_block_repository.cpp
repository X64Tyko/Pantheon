#include <gtest/gtest.h>
#include "db/Database.h"
#include "db/BlockRepository.h"
#include "model/Block.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <string>

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class BlockRepositoryTest : public ::testing::Test {
protected:
    Database        db{ ":memory:" };
    BlockRepository repo{ db };

    void SetUp() override {
        auto& raw = db.get();
        raw.exec("INSERT INTO channel (channel_id, name, number) VALUES ('c1','Test',1)");
        raw.exec("INSERT INTO show  (show_id, title) VALUES ('s1','Show One')");
        raw.exec("INSERT INTO movie (movie_id, title, file_path, duration_ms)"
                 " VALUES ('m1','Movie One','/m.mkv',7200000)");
    }

    void insertBlock(const std::string& bid, const std::string& cid = "c1",
                     const std::string& type = "episode") {
        SQLite::Statement s(db.get(),
            "INSERT INTO block (block_id, channel_id, block_type, start_time, day_mask)"
            " VALUES (?,?,?,'00:00',127)");
        s.bind(1, bid); s.bind(2, cid); s.bind(3, type);
        s.exec();
    }

    void addContent(const std::string& bid, const std::string& ct,
                    const std::string& cid, int pos) {
        SQLite::Statement s(db.get(),
            "INSERT INTO block_content (block_id, content_type, content_id, position)"
            " VALUES (?,?,?,?)");
        s.bind(1, bid); s.bind(2, ct); s.bind(3, cid); s.bind(4, pos);
        s.exec();
    }
};

// ---------------------------------------------------------------------------
// loadBlock
// ---------------------------------------------------------------------------

TEST_F(BlockRepositoryTest, LoadBlock_ReturnsNulloptForUnknownId) {
    EXPECT_FALSE(repo.loadBlock("no_such_block").has_value());
}

TEST_F(BlockRepositoryTest, LoadBlock_ReturnsBlockWithCorrectFields) {
    db.get().exec(
        "INSERT INTO block (block_id, channel_id, block_type, start_time, day_mask,"
        " play_style, advancement, cursor_scope, smart_pct, max_consecutive_episodes,"
        " priority, program_count)"
        " VALUES ('b1','c1','episode','08:30',62,"
        " 'rerun','shuffle','channel',42,3,7,10)");

    auto opt = repo.loadBlock("b1");
    ASSERT_TRUE(opt.has_value());
    const Block& b = *opt;

    EXPECT_EQ(b.block_id,             "b1");
    EXPECT_EQ(b.channel_id,           "c1");
    EXPECT_EQ(b.block_type,           BlockType::Episode);
    EXPECT_EQ(b.start_time,           "08:30");
    EXPECT_EQ(b.day_mask,             62);
    EXPECT_EQ(b.play_style,           PlayStyle::Rerun);
    EXPECT_EQ(b.advancement,          Advancement::Shuffle);
    EXPECT_EQ(b.cursor_scope,         CursorScope::Channel);
    EXPECT_EQ(b.smart_pct,            42);
    EXPECT_EQ(b.max_consecutive_episodes, 3);
    EXPECT_EQ(b.priority,             7);
    EXPECT_EQ(b.program_count,        10);
}

TEST_F(BlockRepositoryTest, LoadBlock_ContentItemsLoadedInPositionOrder) {
    db.get().exec("INSERT INTO show (show_id, title) VALUES ('s2','Show Two')");
    insertBlock("b1");
    addContent("b1", "movie", "m1", 0);
    addContent("b1", "show",  "s2", 1);
    addContent("b1", "show",  "s1", 2);

    auto opt = repo.loadBlock("b1");
    ASSERT_TRUE(opt.has_value());
    const auto& c = opt->content;
    ASSERT_EQ(c.size(), 3u);
    EXPECT_EQ(c[0].position, 0);
    EXPECT_EQ(c[1].position, 1);
    EXPECT_EQ(c[2].position, 2);
}

TEST_F(BlockRepositoryTest, LoadBlock_FillerEntriesLoaded) {
    insertBlock("b1");
    db.get().exec(
        "INSERT INTO block_filler_entry"
        " (block_id, content_type, content_id, advancement, weight, position)"
        " VALUES ('b1','show','s1','shuffle',2,0)");

    auto opt = repo.loadBlock("b1");
    ASSERT_TRUE(opt.has_value());
    ASSERT_EQ(opt->filler_entries.size(), 1u);
    EXPECT_EQ(opt->filler_entries[0].content_type, "show");
    EXPECT_EQ(opt->filler_entries[0].content_id,   "s1");
    EXPECT_EQ(opt->filler_entries[0].advancement,  "shuffle");
    EXPECT_EQ(opt->filler_entries[0].weight,       2);
}

TEST_F(BlockRepositoryTest, LoadBlock_NoContentYieldsEmptyVectors) {
    insertBlock("b1");
    auto opt = repo.loadBlock("b1");
    ASSERT_TRUE(opt.has_value());
    EXPECT_TRUE(opt->content.empty());
    EXPECT_TRUE(opt->filler_entries.empty());
}

TEST_F(BlockRepositoryTest, LoadBlock_ParsesAdvancementValues) {
    for (const auto& [adv, expected] : std::vector<std::pair<std::string, Advancement>>{
            {"sequential", Advancement::Sequential},
            {"shuffle",    Advancement::Shuffle},
            {"smart",      Advancement::Smart},
    }) {
        db.get().exec("DELETE FROM block WHERE block_id='bx'");
        SQLite::Statement s(db.get(),
            "INSERT INTO block (block_id, channel_id, block_type, start_time,"
            " day_mask, advancement) VALUES ('bx','c1','episode','00:00',127,?)");
        s.bind(1, adv);
        s.exec();
        auto opt = repo.loadBlock("bx");
        ASSERT_TRUE(opt.has_value()) << "block not found for advancement=" << adv;
        EXPECT_EQ(opt->advancement, expected) << "advancement mismatch for: " << adv;
    }
}

TEST_F(BlockRepositoryTest, LoadBlock_ChannelIdPopulated) {
    insertBlock("b1", "c1");
    auto opt = repo.loadBlock("b1");
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->channel_id, "c1");
}
