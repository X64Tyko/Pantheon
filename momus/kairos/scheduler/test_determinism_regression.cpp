#include <gtest/gtest.h>
#include "db/Database.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>
#include <string>

// ---------------------------------------------------------------------------
// Regression coverage for the "preview looks different after navigating away
// and back" bug: EPGMaterializer::generate() is supposed to be a pure
// function of (seed, anchor) — no live DB state should leak into what it
// picks. Two bugs broke that guarantee:
//
//  1. Rerun-mode show position and Smart-cooldown recency used to be derived
//     from live play_history/filler_play_history queries instead of the
//     anchor snapshot, so a commit() happening between two generate() calls
//     (e.g. a background /now poll) could silently change a later preview.
//  2. Once (1) was fixed by having project() capture an anchor at every week
//     boundary, a *second* bug surfaced: EPGMaterializer's anchor-key
//     computation used naive UTC-calendar-week arithmetic while project()'s
//     internal week-walk is deliberately timezone-aware — for a channel not
//     on UTC those two "Monday" definitions can land on different
//     timestamps, so a later generate() call could miss the anchor entirely
//     or, worse, overwrite the pristine bootstrap snapshot with
//     partially-projected state.
//
// These tests exercise generate()/commit() directly (not project() in
// isolation) because bug (2) only manifests through that interaction, and
// specifically parameterize over channel timezone because bug (2) is
// invisible on UTC channels.
// ---------------------------------------------------------------------------

class DeterminismRegressionTest : public ::testing::Test {
protected:
    Database        db{ ":memory:" };
    RuleEngine      engine{ db };
    EPGMaterializer materializer{ db, engine };

    // Show with 6 episodes so a multi-day/multi-week horizon has real content
    // to cycle through, plus a "sized" channel-filler entry pulling from the
    // same show — this is what actually caught bug (1) on real data (Smart/
    // filler recency, not just rerun position).
    void setupChannel(const std::string& channel_id, const std::string& tz) {
        auto& raw = db.get();
        SQLite::Statement chIns(raw,
            "INSERT INTO channel (channel_id, name, number, timezone) VALUES (?,?,?,?)");
        chIns.bind(1, channel_id); chIns.bind(2, "Test " + channel_id);
        chIns.bind(3, 1); chIns.bind(4, tz);
        chIns.exec();

        raw.exec("INSERT INTO show (show_id, title) VALUES ('s1','Test Show')");
        for (int i = 1; i <= 6; ++i) {
            SQLite::Statement epIns(raw,
                "INSERT INTO episode (episode_id,show_id,season,episode,title,file_path,duration_ms)"
                " VALUES (?,?,?,?,?,?,?)");
            epIns.bind(1, "e" + std::to_string(i));
            epIns.bind(2, "s1");
            epIns.bind(3, 1);
            epIns.bind(4, i);
            epIns.bind(5, "Episode " + std::to_string(i));
            epIns.bind(6, "/e" + std::to_string(i) + ".mkv");
            epIns.bind(7, static_cast<int64_t>(1800000)); // 30 min
            epIns.exec();
        }

        SQLite::Statement blkIns(raw,
            "INSERT INTO block (block_id,channel_id,block_type,start_time,day_mask,priority,program_count)"
            " VALUES (?,?,?,?,?,?,?)");
        blkIns.bind(1, "b1"); blkIns.bind(2, channel_id); blkIns.bind(3, "episode");
        blkIns.bind(4, "00:00"); blkIns.bind(5, 127); blkIns.bind(6, 0); blkIns.bind(7, 0);
        blkIns.exec();
        raw.exec("UPDATE block SET play_style='rerun', advancement='shuffle' WHERE block_id='b1'");

        SQLite::Statement contentIns(raw,
            "INSERT INTO block_content (block_id,content_type,content_id) VALUES ('b1','show','s1')");
        contentIns.exec();

        SQLite::Statement fillerIns(raw,
            "INSERT INTO channel_filler_entry (channel_id,content_type,content_id,advancement,weight)"
            " VALUES (?,?,?,?,?)");
        fillerIns.bind(1, channel_id); fillerIns.bind(2, "show"); fillerIns.bind(3, "s1");
        fillerIns.bind(4, "sized"); fillerIns.bind(5, 1);
        fillerIns.exec();
    }

    // Runs the exact regression scenario: generate a long-horizon preview,
    // then simulate a background poll (a short-horizon generate()+commit(),
    // like what /now does), then generate the long-horizon preview again.
    // Asserts every item in the overlapping range is identical.
    void assertCommitDoesNotChangePreview(const std::string& channel_id) {
        auto now = std::time(nullptr);

        auto before = materializer.generate(channel_id, now, 336); // 14 days
        ASSERT_FALSE(before.items.empty());

        auto live = materializer.generate(channel_id, now, 4);
        materializer.commit(channel_id, now, now + 4 * 3600, live);

        auto after = materializer.generate(channel_id, now, 336);
        ASSERT_FALSE(after.items.empty());

        size_t n = std::min(before.items.size(), after.items.size());
        for (size_t i = 0; i < n; ++i) {
            ASSERT_EQ(before.items[i].item_id, after.items[i].item_id)
                << "channel=" << channel_id << " item " << i
                << " diverged after an unrelated short-horizon commit "
                << "(start_ms=" << before.items[i].wall_clock_start_ms << ")";
            ASSERT_EQ(before.items[i].wall_clock_start_ms, after.items[i].wall_clock_start_ms)
                << "channel=" << channel_id << " item " << i << " start time diverged";
        }
    }
};

TEST_F(DeterminismRegressionTest, RepeatedGenerateWithNoCommitIsIdentical_UTC) {
    setupChannel("c1", "UTC");
    auto now = std::time(nullptr);
    auto r1  = materializer.generate("c1", now, 336);
    auto r2  = materializer.generate("c1", now, 336);
    ASSERT_FALSE(r1.items.empty());
    ASSERT_EQ(r1.items.size(), r2.items.size());
    for (size_t i = 0; i < r1.items.size(); ++i)
        EXPECT_EQ(r1.items[i].item_id, r2.items[i].item_id) << "item " << i;
}

TEST_F(DeterminismRegressionTest, CommitBetweenGeneratesDoesNotChangePreview_UTC) {
    setupChannel("c1", "UTC");
    assertCommitDoesNotChangePreview("c1");
}

// The interesting case: a channel far enough from UTC that the naive
// UTC-calendar-week boundary and the channel's own timezone-local Monday
// land on different real-world timestamps (see file header, bug 2).
TEST_F(DeterminismRegressionTest, CommitBetweenGeneratesDoesNotChangePreview_NonUTC) {
    setupChannel("c1", "America/Los_Angeles");
    assertCommitDoesNotChangePreview("c1");
}

TEST_F(DeterminismRegressionTest, CommitBetweenGeneratesDoesNotChangePreview_OppositeOffset) {
    setupChannel("c1", "Pacific/Auckland");
    assertCommitDoesNotChangePreview("c1");
}

// A project() call spanning multiple weeks (a long preview, or the
// divergence-checker's probe) must capture an anchor at *every* week
// boundary it crosses, not just the channel's very first week — otherwise
// cursor/RNG continuity silently resets whenever real time crosses into a
// week nothing has anchored yet.
TEST_F(DeterminismRegressionTest, MultiWeekPreviewCapturesAnchorPerWeekBoundary) {
    setupChannel("c1", "America/Los_Angeles");
    auto now = std::time(nullptr);
    auto result = materializer.generate("c1", now, 336); // spans multiple week boundaries
    EXPECT_GT(result.anchors.size(), 1u)
        << "a >1 week horizon must capture more than just the bootstrap anchor";
}
