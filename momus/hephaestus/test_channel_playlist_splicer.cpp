// ChannelPlaylistSplicer is the sole writer of a live channel's canonical
// playlist.m3u8/segments — these tests exercise its relay/reveal logic
// directly against real files in a temp directory (no real ffmpeg/producer
// needed: a "pending" segment is just a file the test creates itself, same
// as VodEncodeStream already reads/writes real segment files by convention).
// wall_clock_start_ms is always set relative to real now() (there's no
// injectable clock), so tests describe elapsed time as "N ms ago"/"N ms from
// now" rather than fixed timestamps.

#include <gtest/gtest.h>
#include "stream/ChannelPlaylistSplicer.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
	int64_t nowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	class ChannelPlaylistSplicerTest : public ::testing::Test
	{
	protected:
		std::filesystem::path root, canonical, pendingA, pendingB;

		void SetUp() override
		{
			root = std::filesystem::temp_directory_path() /
				("momus_splicer_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + testName());
			canonical = root / "canonical";
			pendingA  = root / "pending-a";
			pendingB  = root / "pending-b";
			std::filesystem::create_directories(canonical);
			std::filesystem::create_directories(pendingA);
			std::filesystem::create_directories(pendingB);
		}

		void TearDown() override
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		std::string testName() const
		{
			return ::testing::UnitTest::GetInstance()->current_test_info()->name();
		}

		// Writes a dummy pending segment file — content is irrelevant, the
		// splicer only ever hard-links/relays it, never reads it.
		void writeSegment(const std::filesystem::path& dir, int index)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "seg-%05d.ts", index);
			std::ofstream(dir / buf) << "x";
		}

		std::unique_ptr<ChannelPlaylistSplicer> makeSplicer()
		{
			return std::make_unique<ChannelPlaylistSplicer>(
				canonical.string(), /*list_size=*/12, /*delete_threshold=*/8,
				/*reveal_lead_ms=*/6'000, std::chrono::milliseconds(500));
		}
	};
} // namespace

TEST_F(ChannelPlaylistSplicerTest, RevealsSegmentsAlreadyDueAtSpliceTime)
{
	// 2s segments, item started 10s ago — segments 0-4 (0,2,4,6,8s in) are
	// all due even before the reveal lead is added; segment 5 (10s in) isn't
	// quite due yet at exactly t=10s elapsed with no lead margin spent on it.
	writeSegment(pendingA, 0);
	writeSegment(pendingA, 1);
	writeSegment(pendingA, 2);

	auto splicer = makeSplicer();
	ChannelPlaylistSplicer::SpawnInfo info;
	info.pending_dir           = pendingA.string();
	info.segment_prefix        = "seg-";
	info.segment_boundaries_ms = {0, 2000, 4000};
	info.item_duration_ms      = 6000;
	info.wall_clock_start_ms   = nowMs() - 10'000;
	splicer->spliceTo(info);

	std::string playlist = splicer->canonicalPlaylistForTest();
	EXPECT_NE(playlist.find("seg-00000.ts"), std::string::npos);
	EXPECT_NE(playlist.find("seg-00001.ts"), std::string::npos);
	EXPECT_NE(playlist.find("seg-00002.ts"), std::string::npos);
	// Original pending files relayed away (hard-linked + pruned from pending).
	EXPECT_FALSE(std::filesystem::exists(pendingA / "seg-00000.ts"));
	EXPECT_TRUE(std::filesystem::exists(canonical / "seg-00000.ts"));
}

TEST_F(ChannelPlaylistSplicerTest, HoldsBackSegmentsNotYetDueEvenWithLeadMargin)
{
	// Item started 1s ago; reveal_lead_ms=6000 means "due" covers up to 7s
	// in — segment at boundary 8000ms should NOT be revealed yet.
	writeSegment(pendingA, 0);
	writeSegment(pendingA, 1);

	auto splicer = makeSplicer();
	ChannelPlaylistSplicer::SpawnInfo info;
	info.pending_dir           = pendingA.string();
	info.segment_prefix        = "seg-";
	info.segment_boundaries_ms = {0, 8000};
	info.item_duration_ms      = 16000;
	info.wall_clock_start_ms   = nowMs() - 1'000;
	splicer->spliceTo(info);

	std::string playlist = splicer->canonicalPlaylistForTest();
	EXPECT_NE(playlist.find("seg-00000.ts"), std::string::npos);
	EXPECT_EQ(playlist.find("seg-00001.ts"), std::string::npos);
	// Still sitting in pending — never relayed.
	EXPECT_TRUE(std::filesystem::exists(pendingA / "seg-00001.ts"));
}

TEST_F(ChannelPlaylistSplicerTest, DoesNotSkipAheadWhenAReadySegmentIsMissing)
{
	// Segment 0 exists, segment 1 doesn't (producer hasn't caught up yet),
	// segment 2 does — must not skip past the gap even though both 0 and 2
	// are "due" by wall clock.
	writeSegment(pendingA, 0);
	writeSegment(pendingA, 2);

	auto splicer = makeSplicer();
	ChannelPlaylistSplicer::SpawnInfo info;
	info.pending_dir           = pendingA.string();
	info.segment_prefix        = "seg-";
	info.segment_boundaries_ms = {0, 2000, 4000};
	info.item_duration_ms      = 6000;
	info.wall_clock_start_ms   = nowMs() - 10'000;
	splicer->spliceTo(info);

	std::string playlist = splicer->canonicalPlaylistForTest();
	EXPECT_NE(playlist.find("seg-00000.ts"), std::string::npos);
	EXPECT_EQ(playlist.find("seg-00001.ts"), std::string::npos);

	// Producer catches up — segment 1 appears, next tick relays 1 and 2.
	writeSegment(pendingA, 1);
	splicer->relayTickForTest();
	playlist = splicer->canonicalPlaylistForTest();
	EXPECT_NE(playlist.find("seg-00001.ts"), std::string::npos);
}

TEST_F(ChannelPlaylistSplicerTest, SpliceToInsertsDiscontinuityAndCatchesUpNewSourceBacklog)
{
	writeSegment(pendingA, 0);
	auto splicer = makeSplicer();
	ChannelPlaylistSplicer::SpawnInfo a;
	a.pending_dir           = pendingA.string();
	a.segment_prefix        = "seg-";
	a.segment_boundaries_ms = {0};
	a.item_duration_ms      = 2000;
	a.wall_clock_start_ms   = nowMs() - 5'000;
	splicer->spliceTo(a);

	// New item's producer has already built a backlog (pre-roll) by the
	// time the splice happens — both its segments should relay immediately.
	writeSegment(pendingB, 0);
	writeSegment(pendingB, 1);
	ChannelPlaylistSplicer::SpawnInfo b;
	b.pending_dir           = pendingB.string();
	b.segment_prefix        = "seg-";
	b.segment_boundaries_ms = {0, 2000};
	b.item_duration_ms      = 4000;
	b.wall_clock_start_ms   = nowMs() - 5'000;
	splicer->spliceTo(b);

	std::string playlist = splicer->canonicalPlaylistForTest();
	EXPECT_NE(playlist.find("#EXT-X-DISCONTINUITY\n"), std::string::npos);
	// Canonical numbering is continuous across the splice (seg-00001 is B's
	// first segment, not a restart at seg-00000).
	EXPECT_NE(playlist.find("seg-00001.ts"), std::string::npos);
	EXPECT_NE(playlist.find("seg-00002.ts"), std::string::npos);
}

TEST_F(ChannelPlaylistSplicerTest, PrunesOldSegmentsBeyondListSizePlusDeleteThreshold)
{
	auto splicer = std::make_unique<ChannelPlaylistSplicer>(
		canonical.string(), /*list_size=*/2, /*delete_threshold=*/1,
		/*reveal_lead_ms=*/6'000, std::chrono::milliseconds(500));

	ChannelPlaylistSplicer::SpawnInfo info;
	info.pending_dir         = pendingA.string();
	info.segment_prefix      = "seg-";
	info.item_duration_ms    = 20'000;
	info.wall_clock_start_ms = nowMs() - 20'000; // everything already due

	for (int i = 0; i < 5; ++i)
	{
		info.segment_boundaries_ms.push_back(int64_t(i) * 1000);
		writeSegment(pendingA, i);
	}
	splicer->spliceTo(info);

	// list_size(2) + delete_threshold(1) = 3 retained on disk; older ones
	// physically removed from canonical.
	EXPECT_FALSE(std::filesystem::exists(canonical / "seg-00000.ts"));
	EXPECT_FALSE(std::filesystem::exists(canonical / "seg-00001.ts"));
	EXPECT_TRUE(std::filesystem::exists(canonical / "seg-00004.ts"));

	// Only the newest list_size(2) appear in the playlist text itself.
	std::string playlist = splicer->canonicalPlaylistForTest();
	EXPECT_EQ(playlist.find("seg-00002.ts"), std::string::npos);
	EXPECT_NE(playlist.find("seg-00003.ts"), std::string::npos);
	EXPECT_NE(playlist.find("seg-00004.ts"), std::string::npos);
}