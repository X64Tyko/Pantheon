#include <gtest/gtest.h>
#include "db/Database.h"
#include "scheduler/RuleEngine.h"
#include "model/Block.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Known test constants
//
// 2024-01-01 12:00:00 UTC — a Monday.
//   tm_wday  = 1  →  dayBit = 1 << 1 = 2
//   Minutes from midnight = 720
//   Unix timestamp = 1704110400
// ---------------------------------------------------------------------------

static constexpr std::time_t kMonNoon    = 1704110400;
static constexpr int         kAllDays    = 127; // bits 0-6 = every day
static constexpr int         kMondayOnly = 2;   // 1 << 1
static constexpr int         kSundayOnly = 1;   // 1 << 0
static constexpr int         kTuesdayOnly = 4;  // 1 << 2

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class RuleEngineTest : public ::testing::Test {
protected:
    Database    db{ ":memory:" };
    RuleEngine  engine{ db };

    void SetUp() override {
        auto& raw = db.get();
        raw.exec("INSERT INTO channel (channel_id, name, number) VALUES ('c1','Test Ch',1)");
        raw.exec("INSERT INTO show (show_id, title) VALUES ('s1','Test Show')");
        raw.exec("INSERT INTO episode (episode_id,show_id,season,episode,title,file_path,duration_ms)"
                 " VALUES ('e1','s1',1,1,'Pilot','/e1.mkv',3600000)");
        raw.exec("INSERT INTO episode (episode_id,show_id,season,episode,title,file_path,duration_ms)"
                 " VALUES ('e2','s1',1,2,'Episode 2','/e2.mkv',3600000)");
        raw.exec("INSERT INTO episode (episode_id,show_id,season,episode,title,file_path,duration_ms)"
                 " VALUES ('e3','s1',1,3,'Episode 3','/e3.mkv',3600000)");
        raw.exec("INSERT INTO movie (movie_id,title,file_path,duration_ms)"
                 " VALUES ('m1','Test Movie','/m1.mkv',7200000)");
    }

    void insertBlock(const std::string& bid, const std::string& type,
                     const std::string& start,
                     const std::optional<std::string>& end = std::nullopt,
                     int day_mask = kAllDays, int priority = 0) {
        SQLite::Statement s(db.get(),
            "INSERT INTO block (block_id,channel_id,block_type,start_time,end_time,day_mask,priority)"
            " VALUES (?,?,?,?,?,?,?)");
        s.bind(1, bid); s.bind(2, "c1"); s.bind(3, type); s.bind(4, start);
        if (end.has_value()) s.bind(5, *end); else s.bind(5);
        s.bind(6, day_mask); s.bind(7, priority);
        s.exec();
    }

    void addContent(const std::string& bid, const std::string& type, const std::string& id,
                    const std::optional<int>& season_filter = std::nullopt) {
        SQLite::Statement s(db.get(),
            "INSERT INTO block_content (block_id,content_type,content_id,season_filter)"
            " VALUES (?,?,?,?)");
        s.bind(1, bid); s.bind(2, type); s.bind(3, id);
        if (season_filter.has_value()) s.bind(4, *season_filter); else s.bind(4);
        s.exec();
    }

    void addContentWeighted(const std::string& bid, const std::string& type,
                            const std::string& id, int weight, int run_count = 1) {
        SQLite::Statement s(db.get(),
            "INSERT INTO block_content (block_id,content_type,content_id,weight,run_count)"
            " VALUES (?,?,?,?,?)");
        s.bind(1, bid); s.bind(2, type); s.bind(3, id); s.bind(4, weight); s.bind(5, run_count);
        s.exec();
    }
};

// ---------------------------------------------------------------------------
// loadBlocks
// ---------------------------------------------------------------------------

TEST_F(RuleEngineTest, LoadBlocks_EmptyWhenNoBlocks) {
    EXPECT_TRUE(engine.loadBlocks("c1").empty());
}

TEST_F(RuleEngineTest, LoadBlocks_ReturnsBlockWithPopulatedContent) {
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "show", "s1");
    addContent("b1", "movie", "m1");

    auto blocks = engine.loadBlocks("c1");
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].block_id,   "b1");
    EXPECT_EQ(blocks[0].block_type, BlockType::Episode);
    ASSERT_EQ(blocks[0].content.size(), 2u);
    EXPECT_EQ(blocks[0].content[0].content_type, "show");
    EXPECT_EQ(blocks[0].content[0].content_id,   "s1");
    EXPECT_EQ(blocks[0].content[1].content_type, "movie");
    EXPECT_EQ(blocks[0].content[1].content_id,   "m1");
}

TEST_F(RuleEngineTest, LoadBlocks_SortedByPriorityDescending) {
    insertBlock("b_low",  "episode", "00:00", std::nullopt, kAllDays, 0);
    insertBlock("b_high", "episode", "00:00", std::nullopt, kAllDays, 10);
    insertBlock("b_mid",  "episode", "00:00", std::nullopt, kAllDays, 5);

    auto blocks = engine.loadBlocks("c1");
    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].priority, 10);
    EXPECT_EQ(blocks[1].priority, 5);
    EXPECT_EQ(blocks[2].priority, 0);
}

TEST_F(RuleEngineTest, LoadBlocks_ParsesAllBlockTypes) {
    insertBlock("b_ep",   "episode", "00:00");
    insertBlock("b_fi",   "filler",  "00:00");
    insertBlock("b_mv",   "movie",   "00:00");

    auto blocks = engine.loadBlocks("c1");
    ASSERT_EQ(blocks.size(), 3u);
    std::set<BlockType> types;
    for (const auto& b : blocks) types.insert(b.block_type);
    EXPECT_GT(types.count(BlockType::Episode), 0u);
    EXPECT_GT(types.count(BlockType::Filler),  0u);
    EXPECT_GT(types.count(BlockType::Movie),   0u);
}

// ---------------------------------------------------------------------------
// resolveBlock
// ---------------------------------------------------------------------------

TEST_F(RuleEngineTest, ResolveBlock_NoBlocksReturnsNullopt) {
    EXPECT_FALSE(engine.resolveBlock("c1", kMonNoon).has_value());
}

TEST_F(RuleEngineTest, ResolveBlock_FindsBlockActiveAtGivenTime) {
    insertBlock("b1", "episode", "08:00"); // starts 08:00, no end, all days
    auto block = engine.resolveBlock("c1", kMonNoon); // 12:00 Mon
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(block->block_id, "b1");
}

TEST_F(RuleEngineTest, ResolveBlock_NoMatchBeforeStartTime) {
    insertBlock("b1", "episode", "14:00"); // starts 14:00, we query at 12:00
    EXPECT_FALSE(engine.resolveBlock("c1", kMonNoon).has_value());
}

TEST_F(RuleEngineTest, ResolveBlock_NoMatchAfterEndTime) {
    insertBlock("b1", "episode", "06:00", std::string("10:00")); // 06:00-10:00
    EXPECT_FALSE(engine.resolveBlock("c1", kMonNoon).has_value()); // query at 12:00
}

TEST_F(RuleEngineTest, ResolveBlock_MatchesExactlyAtStartTime) {
    // 12:00 = 720 mins; block starts at 12:00
    insertBlock("b1", "episode", "12:00");
    EXPECT_TRUE(engine.resolveBlock("c1", kMonNoon).has_value());
}

TEST_F(RuleEngineTest, ResolveBlock_MatchesInsideTimeWindow) {
    insertBlock("b1", "episode", "08:00", std::string("16:00")); // 08:00-16:00
    EXPECT_TRUE(engine.resolveBlock("c1", kMonNoon).has_value()); // query at 12:00
}

TEST_F(RuleEngineTest, ResolveBlock_DayMaskExcludesWrongDay) {
    // kMonNoon is Monday (tm_wday=1, dayBit=2); Sunday mask won't match
    insertBlock("b1", "episode", "00:00", std::nullopt, kSundayOnly);
    EXPECT_FALSE(engine.resolveBlock("c1", kMonNoon).has_value());
}

TEST_F(RuleEngineTest, ResolveBlock_DayMaskMondayOnlyMatchesMonday) {
    insertBlock("b1", "episode", "00:00", std::nullopt, kMondayOnly);
    EXPECT_TRUE(engine.resolveBlock("c1", kMonNoon).has_value());
}

TEST_F(RuleEngineTest, ResolveBlock_DayMaskTuesdayOnlyNoMatchOnMonday) {
    insertBlock("b1", "episode", "00:00", std::nullopt, kTuesdayOnly);
    EXPECT_FALSE(engine.resolveBlock("c1", kMonNoon).has_value());
}

TEST_F(RuleEngineTest, ResolveBlock_HigherPriorityBlockWins) {
    insertBlock("b_low",  "episode", "08:00", std::nullopt, kAllDays, 0);
    insertBlock("b_high", "episode", "08:00", std::nullopt, kAllDays, 10);
    addContent("b_low",  "show", "s1");
    addContent("b_high", "show", "s1");
    auto block = engine.resolveBlock("c1", kMonNoon);
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(block->block_id, "b_high");
}

TEST_F(RuleEngineTest, ResolveBlock_LowerPriorityBlockUsedWhenHigherNotActive) {
    // High priority block only active 08:00-10:00; query at 12:00 → falls through to low
    insertBlock("b_high", "episode", "08:00", std::string("10:00"), kAllDays, 10);
    insertBlock("b_low",  "episode", "00:00", std::nullopt,          kAllDays, 0);
    auto block = engine.resolveBlock("c1", kMonNoon);
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(block->block_id, "b_low");
}

// ---------------------------------------------------------------------------
// nextItem — peek semantics
// ---------------------------------------------------------------------------

TEST_F(RuleEngineTest, NextItem_EmptyBlockReturnsNullopt) {
    insertBlock("b1", "episode", "08:00");
    // no content added
    auto blocks = engine.loadBlocks("c1");
    ASSERT_FALSE(blocks.empty());
    EXPECT_FALSE(engine.nextItem("c1", blocks[0], kMonNoon).has_value());
}

TEST_F(RuleEngineTest, NextItem_ShowReturnsFirstEpisodeAtCursorZero) {
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "show", "s1");
    auto blocks = engine.loadBlocks("c1");
    auto item = engine.nextItem("c1", blocks[0], kMonNoon);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->item_type,   "episode");
    EXPECT_EQ(item->item_id,     "e1");
    EXPECT_EQ(item->show_title,  "Test Show");
    EXPECT_EQ(item->season,      1);
    EXPECT_EQ(item->episode_num, 1);
    EXPECT_EQ(item->channel_id,  "c1");
    EXPECT_EQ(item->block_id,    "b1");
}

TEST_F(RuleEngineTest, NextItem_IsPeekDoesNotAdvanceCursor) {
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "show", "s1");
    auto blocks = engine.loadBlocks("c1");
    auto item1  = engine.nextItem("c1", blocks[0], kMonNoon);
    auto item2  = engine.nextItem("c1", blocks[0], kMonNoon);
    ASSERT_TRUE(item1.has_value());
    ASSERT_TRUE(item2.has_value());
    EXPECT_EQ(item1->item_id, item2->item_id) << "nextItem must be a pure peek";
}

TEST_F(RuleEngineTest, NextItem_MovieBlock) {
    insertBlock("b1", "movie", "08:00");
    addContent("b1", "movie", "m1");
    auto blocks = engine.loadBlocks("c1");
    auto item = engine.nextItem("c1", blocks[0], kMonNoon);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->item_type,   "movie");
    EXPECT_EQ(item->item_id,     "m1");
    EXPECT_EQ(item->title,       "Test Movie");
    EXPECT_EQ(item->duration_ms, 7200000);
}

TEST_F(RuleEngineTest, NextItem_DirectEpisodeContent) {
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "episode", "e2"); // specific episode, not whole show
    auto blocks = engine.loadBlocks("c1");
    auto item = engine.nextItem("c1", blocks[0], kMonNoon);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->item_id,  "e2");
    EXPECT_EQ(item->show_id,  "s1");
    EXPECT_EQ(item->season,   1);
    EXPECT_EQ(item->episode_num, 2);
}

TEST_F(RuleEngineTest, NextItem_SeasonFilterLimitsEpisodes) {
    // Add season 2 episodes
    db.get().exec("INSERT INTO episode (episode_id,show_id,season,episode,title,file_path,duration_ms)"
                  " VALUES ('s2e1','s1',2,1,'S2 Pilot','/s2e1.mkv',3600000)");
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "show", "s1", std::optional<int>(2)); // season_filter=2
    auto blocks = engine.loadBlocks("c1");
    auto item = engine.nextItem("c1", blocks[0], kMonNoon);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->season, 2) << "season_filter should restrict to season 2 only";
}

// ---------------------------------------------------------------------------
// markPlayed — cursor advancement depends on advance_mode
// ---------------------------------------------------------------------------
// In 'scheduled' mode (default), project() owns cursor advancement; markPlayed()
// records history only. In 'on_play' mode, markPlayed() is the sole cursor advance.

TEST_F(RuleEngineTest, MarkPlayed_InsertsPlayHistoryEntry) {
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "show", "s1");
    engine.markPlayed("c1", "b1", "episode", "e1", 3600000);
    SQLite::Statement q(db.get(),
        "SELECT COUNT(*) FROM play_history WHERE item_id='e1' AND channel_id='c1'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 1);
}

TEST_F(RuleEngineTest, MarkPlayed_Scheduled_DoesNotAdvanceCursor) {
    // 'scheduled' mode: project() drives cursors; markPlayed() must not double-advance.
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "show", "s1");

    auto first = engine.nextItem("c1", engine.loadBlocks("c1")[0], kMonNoon);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->item_id, "e1");

    engine.markPlayed("c1", "b1", "episode", "e1", 3600000);

    auto still_first = engine.nextItem("c1", engine.loadBlocks("c1")[0], kMonNoon);
    ASSERT_TRUE(still_first.has_value());
    EXPECT_EQ(still_first->item_id, "e1") << "scheduled mode: cursor must not advance on markPlayed";
}

TEST_F(RuleEngineTest, MarkPlayed_OnPlay_AdvancesShowCursorToNextEpisode) {
    db.get().exec("UPDATE channel SET advance_mode='on_play' WHERE channel_id='c1'");
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "show", "s1");

    auto first = engine.nextItem("c1", engine.loadBlocks("c1")[0], kMonNoon);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->item_id, "e1");

    engine.markPlayed("c1", "b1", "episode", "e1", 3600000);

    auto second = engine.nextItem("c1", engine.loadBlocks("c1")[0], kMonNoon);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->item_id, "e2") << "on_play mode: cursor should have advanced to e2";
}

TEST_F(RuleEngineTest, MarkPlayed_OnPlay_WrapsAroundToFirstEpisodeAfterLast) {
    db.get().exec("UPDATE channel SET advance_mode='on_play' WHERE channel_id='c1'");
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "show", "s1");

    engine.markPlayed("c1", "b1", "episode", "e1", 3600000);
    engine.markPlayed("c1", "b1", "episode", "e2", 3600000);
    engine.markPlayed("c1", "b1", "episode", "e3", 3600000);

    auto item = engine.nextItem("c1", engine.loadBlocks("c1")[0], kMonNoon);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->item_id, "e1") << "should wrap back to first episode";
}

TEST_F(RuleEngineTest, MarkPlayed_MultipleHistoryEntriesAccumulate) {
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "show", "s1");
    engine.markPlayed("c1", "b1", "episode", "e1", 3600000);
    engine.markPlayed("c1", "b1", "episode", "e2", 3600000);
    SQLite::Statement q(db.get(),
        "SELECT COUNT(*) FROM play_history WHERE channel_id='c1'");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 2);
}

TEST_F(RuleEngineTest, MarkPlayed_OnPlay_RoundRobinAdvancesThroughContentItems) {
    db.get().exec("UPDATE channel SET advance_mode='on_play' WHERE channel_id='c1'");
    insertBlock("b1", "episode", "08:00");
    addContent("b1", "show",  "s1");
    addContent("b1", "movie", "m1");

    // block_rr=0 → show → nextItem returns episode
    auto item1 = engine.nextItem("c1", engine.loadBlocks("c1")[0], kMonNoon);
    ASSERT_TRUE(item1.has_value());
    EXPECT_EQ(item1->item_type, "episode");

    engine.markPlayed("c1", "b1", "episode", "e1", 3600000);
    // block_rr=1 → movie
    auto item2 = engine.nextItem("c1", engine.loadBlocks("c1")[0], kMonNoon);
    ASSERT_TRUE(item2.has_value());
    EXPECT_EQ(item2->item_type, "movie");

    engine.markPlayed("c1", "b1", "movie", "m1", 7200000);
    // block_rr wraps to 0 → show again
    auto item3 = engine.nextItem("c1", engine.loadBlocks("c1")[0], kMonNoon);
    ASSERT_TRUE(item3.has_value());
    EXPECT_EQ(item3->item_type, "episode");
}

// ---------------------------------------------------------------------------
// project — EPG forward projection
// ---------------------------------------------------------------------------

TEST_F(RuleEngineTest, Project_EmptyWhenNoBlocks) {
    Xoshiro256 rng(0);
    EXPECT_TRUE(engine.project("c1", kMonNoon, 2, rng).empty());
}

TEST_F(RuleEngineTest, Project_ProducesItemsInChronologicalOrder) {
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");
    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 2, rng);
    ASSERT_FALSE(items.empty());
    for (size_t i = 1; i < items.size(); ++i)
        EXPECT_GE(items[i].wall_clock_start_ms, items[i-1].wall_clock_end_ms)
            << "Items must be chronologically ordered without overlap";
}

TEST_F(RuleEngineTest, Project_FirstItemStartsAtRequestedTime) {
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");
    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 1, rng);
    ASSERT_FALSE(items.empty());
    EXPECT_EQ(items[0].wall_clock_start_ms, static_cast<int64_t>(kMonNoon) * 1000);
}

TEST_F(RuleEngineTest, Project_ItemsHaveCorrectChannelAndBlockIds) {
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");
    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 1, rng);
    ASSERT_FALSE(items.empty());
    for (const auto& item : items) {
        EXPECT_EQ(item.channel_id, "c1");
        EXPECT_EQ(item.block_id,   "b1");
    }
}

TEST_F(RuleEngineTest, Project_DeterministicWithSameSeed) {
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");
    Xoshiro256 rng1(7);
    auto run1 = engine.project("c1", kMonNoon, 4, rng1);

    // In production, the anchor system restores cursor/block_state rows before
    // each projection call. Simulate that here by clearing the state run1 wrote.
    db.get().exec("DELETE FROM block_state   WHERE channel_id='c1'");
    db.get().exec("DELETE FROM media_cursor  WHERE scope_id='c1'");
    db.get().exec("DELETE FROM play_history  WHERE channel_id='c1'");

    Xoshiro256 rng2(7);
    auto run2 = engine.project("c1", kMonNoon, 4, rng2);
    ASSERT_EQ(run1.size(), run2.size());
    for (size_t i = 0; i < run1.size(); ++i) {
        EXPECT_EQ(run1[i].item_id,             run2[i].item_id);
        EXPECT_EQ(run1[i].wall_clock_start_ms, run2[i].wall_clock_start_ms);
    }
}

TEST_F(RuleEngineTest, Project_DifferentSeedsStartAtDifferentEpisodes) {
    // Sequential/Shuffle/Smart episode selection is deterministic by design —
    // no RNG draw decides the *starting* episode, only later cycling (Shuffle's
    // permutation is seeded from content_id+block_id, not the passed-in rng).
    // Only a Rerun block's free-random phase (FallbackAll skips straight to it)
    // actually draws from the passed-in rng for its very first pick. This test
    // guards against a regression where the rng is threaded through but never
    // actually consumed, so every seed silently produces the same schedule.
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");
    db.get().exec("UPDATE block SET play_style='rerun', no_history_behavior='fallback_all' WHERE block_id='b1'");

    std::set<std::string> distinct_first_picks;
    for (uint64_t seed = 0; seed < 10; ++seed) {
        Xoshiro256 rng(seed);
        auto items = engine.project("c1", kMonNoon, 1, rng);
        ASSERT_FALSE(items.empty());
        distinct_first_picks.insert(items[0].item_id);
    }
    EXPECT_GT(distinct_first_picks.size(), 1u)
        << "different seeds must be able to produce different starting episodes";
}

TEST_F(RuleEngineTest, Project_CursorJsonEnablesContinuousExtension) {
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");

    // project() advances DB cursors as it schedules; a second call starting from
    // the first batch's end automatically picks up from the advanced position.
    Xoshiro256 rng(0);
    auto batch1 = engine.project("c1", kMonNoon, 1, rng);
    ASSERT_FALSE(batch1.empty());

    std::time_t resume_at = batch1.back().wall_clock_end_ms / 1000;
    auto batch2 = engine.project("c1", resume_at, 1, rng);

    if (!batch2.empty())
        EXPECT_GE(batch2[0].wall_clock_start_ms, batch1.back().wall_clock_end_ms)
            << "Second batch must start at or after end of first batch";
}

TEST_F(RuleEngineTest, Project_RespectsHorizonLimit) {
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");
    // 1-hour horizon with 1-hour episodes → at most 1 item
    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 1, rng);
    // Each episode is 3600s; horizon is 3600s → exactly 1 episode fits
    EXPECT_LE(items.size(), 1u);
}

TEST_F(RuleEngineTest, Project_NoItemsWhenBlockInactive) {
    // Block only active 14:00-16:00, but we query starting at 12:00
    insertBlock("b1", "episode", "14:00", std::string("16:00"));
    addContent("b1", "show", "s1");
    // 1-hour projection at 12:00 → block not yet active → no items
    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 1, rng);
    EXPECT_TRUE(items.empty());
}

// ---------------------------------------------------------------------------
// Phase 2a: Weighted show selection for non-rerun modes
// ---------------------------------------------------------------------------

TEST_F(RuleEngineTest, Sequential_WeightRunCount_PlaysNEpisodesPerShowBeforeSwitching) {
    // s1 (weight=2): plays 2 episodes before switching; s2 (weight=1): plays 1.
    auto& raw = db.get();
    raw.exec("INSERT INTO show (show_id, title) VALUES ('s2','Show 2')");
    raw.exec("INSERT INTO episode (episode_id,show_id,season,episode,title,file_path,duration_ms)"
             " VALUES ('e4','s2',1,1,'S2E1','/e4.mkv',3600000)");
    raw.exec("INSERT INTO episode (episode_id,show_id,season,episode,title,file_path,duration_ms)"
             " VALUES ('e5','s2',1,2,'S2E2','/e5.mkv',3600000)");

    insertBlock("b1", "episode", "00:00");
    addContentWeighted("b1", "show", "s1", 2);
    addContentWeighted("b1", "show", "s2", 1);

    // Project 3 episodes (3-hour window, each ep = 1 hr).
    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 3, rng);
    ASSERT_GE(items.size(), 3u);

    const std::set<std::string> s1_eps = {"e1", "e2", "e3"};
    const std::set<std::string> s2_eps = {"e4", "e5"};
    EXPECT_TRUE(s1_eps.count(items[0].item_id)) << "slot 0 should be from s1 (weight=2)";
    EXPECT_TRUE(s1_eps.count(items[1].item_id)) << "slot 1 should be from s1 (weight=2)";
    EXPECT_TRUE(s2_eps.count(items[2].item_id)) << "slot 2 should switch to s2 (weight=1)";
}

TEST_F(RuleEngineTest, Shuffle_WeightedShowSelection_SchedulesWithoutError) {
    // Smoke test: weighted shuffle with 2 shows must schedule items without crashing.
    auto& raw = db.get();
    raw.exec("INSERT INTO show (show_id, title) VALUES ('s2','Show 2')");
    raw.exec("INSERT INTO episode (episode_id,show_id,season,episode,title,file_path,duration_ms)"
             " VALUES ('e4','s2',1,1,'S2E1','/e4.mkv',3600000)");

    insertBlock("b1", "episode", "00:00");
    raw.exec("UPDATE block SET advancement='shuffle' WHERE block_id='b1'");
    addContentWeighted("b1", "show", "s1", 3);
    addContentWeighted("b1", "show", "s2", 1);

    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 4, rng);
    EXPECT_FALSE(items.empty()) << "weighted shuffle should still schedule items";
}

// ---------------------------------------------------------------------------
// Phase 2b: Cursor scope semantic cleanup
// ---------------------------------------------------------------------------

TEST_F(RuleEngineTest, GlobalScope_RerunBlockSchedulesEpisodesPlayedOnOtherChannel) {
    // Exclude-mode rerun eligibility comes purely from CursorState now (no live
    // DB history query) — a show with no cursor at the block's configured scope
    // isn't eligible; it becomes eligible once a cursor exists at that scope.
    // This mirrors the old test's "played on another channel" scenario using
    // cursor_scope=global (scope_id="", shared across every channel) instead of
    // a real cross-channel play_history row.
    //
    // pickNextContent only re-checks eligibility once it's about to *advance*
    // past the current selection (should_advance) — a block's structurally
    // first-ever pick always keeps content_pos as-is regardless of eligibility
    // (there's nothing to advance away from yet). Pre-seed block/runs state so
    // should_advance is already true on the call under test, instead of that
    // bypass.
    auto& raw = db.get();
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");
    // no_history_behavior=skip (alias for exclude): empty pool → no output
    // (avoids Normal mode's fallback to a fresh sequential pass).
    raw.exec("UPDATE block SET play_style='rerun', advancement='shuffle', cursor_scope='channel',"
             " no_history_behavior='skip' WHERE block_id='b1'");

    // Channel-scoped, no cursor seeded at all → rerun pool empty → skip → no items.
    CursorState ch_state;
    ch_state.setBlockPosition("b1", 0, 0, 0);
    Xoshiro256 rng0(0);
    auto ch_items = engine.project("c1", kMonNoon, 1, ch_state, rng0);
    EXPECT_TRUE(ch_items.empty()) << "channel-scoped rerun with no cursor should schedule nothing";

    // Switch to global scope and pre-seed a global-scoped cursor for s1 (as if it
    // had already aired somewhere under this scope) — the pool should include it.
    raw.exec("UPDATE block SET cursor_scope='global' WHERE block_id='b1'");
    CursorState gl_state;
    gl_state.setBlockPosition("b1", 0, 0, 0);
    gl_state.setCursorPos("show", "s1", "global", "", 0, "e1", "season");
    Xoshiro256 rng1(0);
    auto gl_items = engine.project("c1", kMonNoon, 1, gl_state, rng1);
    EXPECT_FALSE(gl_items.empty()) << "global-scoped rerun should see the pre-seeded global cursor";
}

TEST_F(RuleEngineTest, ChannelScope_RerunCursorUsesChannelScopedKey) {
    // With cursor_scope=channel the rerun cursor key should be ("show_rerun", s1, "channel", c1).
    // project() stores cursor state in CursorState; applyToDB() persists it so we can verify.
    // Pre-seed the watermark at the second-to-last episode (e2) so the very
    // first pick (advancing to e3, the last one, with nothing pending in ahead)
    // completes the first pass and seeds a show_rerun cursor — see
    // advanceShowCursor's completion check.
    auto& raw = db.get();
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");
    raw.exec("UPDATE block SET play_style='rerun', advancement='shuffle', cursor_scope='channel' WHERE block_id='b1'");

    Xoshiro256 rng(0);
    CursorState state;
    state.setCursorPos("show", "s1", "channel", "c1", 0, "e2", "season");
    engine.project("c1", kMonNoon, 1, state, rng);
    state.applyToDB(db, "c1");

    // The rerun cursor should be stored with cursor_scope='channel', scope_id='c1'.
    SQLite::Statement q(raw,
        "SELECT cursor_scope, scope_id FROM media_cursor"
        " WHERE content_type='show_rerun' AND content_id='s1'");
    ASSERT_TRUE(q.executeStep()) << "rerun cursor should exist after project()";
    EXPECT_EQ(q.getColumn(0).getString(), "channel");
    EXPECT_EQ(q.getColumn(1).getString(), "c1");
}

// ---------------------------------------------------------------------------
// CursorState integration: advanceAndGet / applyToDB / nextItem continuity
// ---------------------------------------------------------------------------

TEST_F(RuleEngineTest, AdvanceAndGet_NulloptWhenNoContent) {
    // advanceAndGet is tested via project(): a block with no content items must yield
    // no scheduled output regardless of how many hours are projected.
    insertBlock("b1", "episode", "00:00");
    // no content added
    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 2, rng);
    EXPECT_TRUE(items.empty());
}

TEST_F(RuleEngineTest, SmartCooldown_OffByOneFixed) {
    // Movie-only smart block with smart_pct=50 and 2 equally-weighted movies.
    // m1 is in play history (hot); selectWeightedSmartCooldown should exclude m1
    // and always return m2. The old r < 0 bug could accidentally pick m1 when r==0.
    auto& raw = db.get();
    raw.exec("INSERT INTO movie (movie_id,title,file_path,duration_ms)"
             " VALUES ('m2','Movie 2','/m2.mkv',3600000)");
    raw.exec("INSERT INTO play_history (channel_id,item_type,item_id,aired_at,is_scheduled)"
             " VALUES ('c1','movie','m1'," + std::to_string(kMonNoon - 7200) + ",1)");

    insertBlock("b1", "movie", "00:00");
    addContentWeighted("b1", "movie", "m1", 1);
    addContentWeighted("b1", "movie", "m2", 1);
    db.get().exec("UPDATE block SET advancement='smart', smart_pct=50 WHERE block_id='b1'");

    // m1 is 2h (fixture default), m2 is 1h; need a 4h window to fit both.
    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 4, rng);
    ASSERT_GE(items.size(), 2u);
    // items[0] is always m1 (content_pos starts at 0). selectWeightedSmartCooldown
    // sets the NEXT position: m1 is hot so m2 is chosen for items[1].
    EXPECT_EQ(items[1].item_id, "m2")
        << "hot movie m1 must be excluded by smart cooldown; second pick must be m2";
}

TEST_F(RuleEngineTest, MaxConsecutiveEpisodes_NeverExceedsLimit) {
    // Two shows, max_consecutive_episodes=2. No run of the same show_id should exceed 2.
    auto& raw = db.get();
    raw.exec("INSERT INTO show (show_id, title) VALUES ('s2','Show 2')");
    for (int i = 1; i <= 4; ++i) {
        std::string eid = "s2e" + std::to_string(i);
        raw.exec("INSERT INTO episode (episode_id,show_id,season,episode,title,file_path,duration_ms)"
                 " VALUES ('" + eid + "','s2',1," + std::to_string(i) + ",'Ep " + std::to_string(i) + "','/"+eid+".mkv',3600000)");
    }

    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");
    addContent("b1", "show", "s2");
    // play_style='rerun' is required: the consecutive-episode limit is only enforced
    // in the rerun scheduling path; standard advancement uses plain selectWeighted.
    raw.exec("UPDATE block SET play_style='rerun', advancement='smart', max_consecutive_episodes=2 WHERE block_id='b1'");

    Xoshiro256 rng(42);
    auto items = engine.project("c1", kMonNoon, 6, rng);
    ASSERT_GE(items.size(), 4u) << "Need enough items to observe consecutive runs";

    int consecutive = 1;
    for (size_t i = 1; i < items.size(); ++i) {
        if (items[i].show_id == items[i-1].show_id)
            ++consecutive;
        else
            consecutive = 1;
        EXPECT_LE(consecutive, 2)
            << "Consecutive run of " << items[i].show_id << " exceeds max=2 at position " << i;
    }
}

TEST_F(RuleEngineTest, MaxConsecutiveEpisodes_ZeroMeansUnlimited) {
    // max_consecutive_episodes=0: the only show should repeat without being interrupted.
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");
    db.get().exec("UPDATE block SET advancement='shuffle', max_consecutive_episodes=0 WHERE block_id='b1'");

    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 3, rng);
    ASSERT_GE(items.size(), 2u);
    for (const auto& item : items)
        EXPECT_EQ(item.show_id, "s1") << "Only s1 is in the block; all items must be from s1";
}

TEST_F(RuleEngineTest, Project_CursorStateAppliedToDBAfterProject) {
    // After project() + applyToDB(), the media_cursor table must contain an entry
    // reflecting the advanced show cursor (block scope = default).
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");

    // Pre-seed state so the RNG-based seed-init inside project() is skipped.
    // Without this, projectDay randomises the starting episode position, and the
    // cursor wraps to 0 when it happens to start on the last episode.
    CursorState state;
    state.setBlockPosition("b1", 0, 1, 0);   // mark block as initialised
    state.setCursorPos("show", "s1", "block", "b1", 0);  // episode cursor at 0

    Xoshiro256 rng(0);
    auto items = engine.project("c1", kMonNoon, 1, state, rng);
    ASSERT_FALSE(items.empty());
    state.applyToDB(db, "c1");

    // Cursor advanced from 0 → 1 after scheduling e1; must exist in DB with position=1.
    SQLite::Statement q(db.get(),
        "SELECT position FROM media_cursor"
        " WHERE content_id='s1' AND cursor_scope='block' AND scope_id='b1'");
    ASSERT_TRUE(q.executeStep()) << "show cursor must be persisted after applyToDB";
    EXPECT_GT(q.getColumn(0).getInt(), 0) << "cursor position must have advanced past 0";
}

TEST_F(RuleEngineTest, Project_NextItemReflectsCursorStateLoadFromDB) {
    // After a project()+applyToDB() cycle, nextItem() (which loads state from DB)
    // must return the episode after the last projected item, not episode 1 again.
    insertBlock("b1", "episode", "00:00");
    addContent("b1", "show", "s1");

    // Project 1 hour (= 1 episode at 3600s each).
    Xoshiro256 rng(0);
    CursorState state;
    auto items = engine.project("c1", kMonNoon, 1, state, rng);
    ASSERT_FALSE(items.empty());
    const std::string first_id = items[0].item_id;

    state.applyToDB(db, "c1");

    auto blocks  = engine.loadBlocks("c1");
    auto peek    = engine.nextItem("c1", blocks[0], kMonNoon + 7200);
    ASSERT_TRUE(peek.has_value());
    EXPECT_NE(peek->item_id, first_id)
        << "nextItem must reflect the advanced cursor — should not replay the first episode";
}
