// VodEncodeStream orchestrates real ffmpeg-class processes ("heads") across
// concurrent viewers of the same VOD content, and this session's hardening
// pass found and fixed three real bugs in that orchestration: two live heads
// could claim overlapping segment-index ranges and write colliding files
// (fixed via the window-end clamp in spawnHead() and the supersede-eviction
// in prepareSegment()), and a head whose real output outran its own declared
// window (a fallback-boundary case — see VodSession::computeSegmentBoundaries())
// could keep writing into another head's territory until tick() started
// actively detecting and stopping it. None of that had test coverage before
// this file — test_channel_session.cpp's sibling comment explains why
// process-spawning code is usually left untested in this codebase
// (KairosClient isn't mockable), but VodEncodeStream doesn't need Kairos at
// all: its ArgsBuilder is injected, so a test can hand it a trivial,
// long-lived real command ("sleep") instead of a real ffmpeg invocation and
// still exercise the *real* process lifecycle (fork/exec, isAlive(), kill())
// that the collision-prevention logic depends on. Segment "progress" is
// simulated by touching files directly rather than waiting on the fake
// process to produce anything, keeping every test fast and deterministic.

#include <gtest/gtest.h>
#include "stream/VodEncodeStream.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <utility>

namespace
{
	class VodEncodeStreamTest : public ::testing::Test
	{
	protected:
		std::filesystem::path dir;

		void SetUp() override
		{
			dir = std::filesystem::temp_directory_path() /
				("momus_vod_encode_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + testName());
			std::filesystem::create_directories(dir);
		}

		void TearDown() override
		{
			std::error_code ec;
			std::filesystem::remove_all(dir, ec);
		}

		std::string testName() const
		{
			return ::testing::UnitTest::GetInstance()->current_test_info()->name();
		}

		// A stream whose "ffmpeg" is really just a long-lived, always-succeeds
		// real process (sleep 30) — real fork/exec/isAlive()/kill() semantics,
		// zero dependency on ffmpeg/ffprobe/Kairos. 30s comfortably outlives
		// any test here; VodEncodeStream's destructor (via stop()) kills it
		// during TearDown regardless of how far the sleep got.
		std::unique_ptr<VodEncodeStream> makeStream()
		{
			VodEncodeStream::ArgsBuilder args = [](int, int64_t, std::optional<double>)
			{
				return std::vector<std::string>{"sleep", "30"};
			};
			return std::make_unique<VodEncodeStream>("video", dir.string(), "seg-", args,
													 /*buffer_size=*/65536, /*ffmpeg_debug_logs=*/false,
													 /*verbose_transcode_logs=*/false,
													 /*lookahead_secs=*/12, /*hls_time_secs=*/6);
		}

		// Mirrors VodEncodeStream::segmentPath()'s own private formatting
		// exactly (segment_prefix "seg-" + 5-digit zero-padded index + .ts).
		std::filesystem::path segmentPath(int index) const
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "seg-%05d.ts", index);
			return dir / buf;
		}

		void touchSegment(int index) { std::ofstream(segmentPath(index)) << "x"; }

		// window_duration_secs math only needs monotonically increasing
		// values — the exact cadence is irrelevant to every test here, none
		// of which assert on timing.
		std::vector<int64_t> segmentStartMs(int count) const
		{
			std::vector<int64_t> v(static_cast<size_t>(count));
			for (int i = 0; i < count; ++i) v[static_cast<size_t>(i)] = static_cast<int64_t>(i) * 6000;
			return v;
		}
	};
} // namespace

TEST_F(VodEncodeStreamTest, NoLiveHeadsInitially)
{
	auto stream = makeStream();
	EXPECT_EQ(stream->liveHeadCount(), 0);
	EXPECT_TRUE(stream->headWindowsForTest().empty());
}

TEST_F(VodEncodeStreamTest, PrepareSegmentSpawnsHeadCoveringRequestedIndex)
{
	auto stream     = makeStream();
	auto segment_ms = segmentStartMs(250);
	auto result     = stream->prepareSegment(0, segment_ms, 250);

	EXPECT_EQ(result, VodEncodeStream::SegmentPrep::WaitColdStart);
	EXPECT_EQ(stream->liveHeadCount(), 1);
	auto windows = stream->headWindowsForTest();
	ASSERT_EQ(windows.size(), 1u);
	EXPECT_EQ(windows[0].first, 0);
}

TEST_F(VodEncodeStreamTest, AlreadyOnDiskSegmentServedReadyWithoutSpawningAHead)
{
	// Direct regression test for the fix: a file another (possibly already-
	// gone, possibly overrunning) process already produced must be served
	// straight from disk, never triggering a redundant second writer for the
	// same index.
	auto stream = makeStream();
	touchSegment(5);

	auto result = stream->prepareSegment(5, segmentStartMs(250), 250);

	EXPECT_EQ(result, VodEncodeStream::SegmentPrep::Ready);
	EXPECT_EQ(stream->liveHeadCount(), 0);
}

TEST_F(VodEncodeStreamTest, RepeatedRequestWithinCatchUpMarginDoesNotRespawn)
{
	auto stream     = makeStream();
	auto segment_ms = segmentStartMs(250);
	ASSERT_EQ(stream->prepareSegment(0, segment_ms, 250), VodEncodeStream::SegmentPrep::WaitColdStart);
	ASSERT_EQ(stream->liveHeadCount(), 1);

	// Nothing has actually been produced (fake process never writes files),
	// so highest_generated is still -1 — segment 2 is within the head's
	// (not-stalled — it was just spawned) catch-up margin and should just
	// wait on the existing head rather than spawning a second one.
	auto result = stream->prepareSegment(2, segment_ms, 250);

	EXPECT_EQ(result, VodEncodeStream::SegmentPrep::WaitShort);
	EXPECT_EQ(stream->liveHeadCount(), 1);
}

TEST_F(VodEncodeStreamTest, RequestFarAheadOfLaggingHeadEvictsItInsteadOfLettingBothRun)
{
	// The actual head-collision bug this session fixed: before the fix, the
	// original head here was left running (never evicted) once a fresh head
	// was spawned for a far-ahead request within its own declared window —
	// two live heads, one flat filename numbering, guaranteed collision.
	auto stream     = makeStream();
	auto segment_ms = segmentStartMs(250);
	ASSERT_EQ(stream->prepareSegment(0, segment_ms, 250), VodEncodeStream::SegmentPrep::WaitColdStart);
	ASSERT_EQ(stream->liveHeadCount(), 1);

	// Segment 50 is still inside the first head's naive [0,100) window, but
	// far beyond both what it's generated (nothing) and the catch-up margin.
	auto result = stream->prepareSegment(50, segment_ms, 250);

	EXPECT_EQ(result, VodEncodeStream::SegmentPrep::WaitColdStart);
	EXPECT_EQ(stream->liveHeadCount(), 1) << "the lagging head must be evicted, not left running alongside the new one";
	auto windows = stream->headWindowsForTest();
	ASSERT_EQ(windows.size(), 1u);
	EXPECT_EQ(windows[0].first, 50) << "only the new head covering the actual request should remain";
}

TEST_F(VodEncodeStreamTest, NewHeadWindowClampsAgainstALaterUnrelatedHeadsStart)
{
	// The other half of the collision fix: a *different* live head (not the
	// one being superseded) whose start falls inside a freshly spawned
	// head's naive window must clip that new head's window instead of
	// silently overlapping it.
	auto stream     = makeStream();
	auto segment_ms = segmentStartMs(1000);

	ASSERT_EQ(stream->prepareSegment(300, segment_ms, 1000), VodEncodeStream::SegmentPrep::WaitColdStart);
	ASSERT_EQ(stream->liveHeadCount(), 1);

	// 250's naive window is [250,350) — head A already owns [300,400), so
	// this one must clamp to end at exactly 300.
	ASSERT_EQ(stream->prepareSegment(250, segment_ms, 1000), VodEncodeStream::SegmentPrep::WaitColdStart);
	ASSERT_EQ(stream->liveHeadCount(), 2);

	auto windows = stream->headWindowsForTest();
	std::sort(windows.begin(), windows.end());
	ASSERT_EQ(windows.size(), 2u);
	EXPECT_EQ(windows[0], std::make_pair(250, 300)) << "clamped short instead of the naive [250,350)";
	EXPECT_EQ(windows[1], std::make_pair(300, 400));
}

TEST_F(VodEncodeStreamTest, TickDetectsAndStopsAHeadThatOverranItsDeclaredWindow)
{
	// Simulates the fallback-boundary case: real cut cadence denser than the
	// assumed-uniform one a head's window was sized against, so its real
	// output reaches past window_end_segment. Before this session's fix,
	// nothing ever noticed until the whole -t-bounded run finished.
	auto stream     = makeStream();
	auto segment_ms = segmentStartMs(250);
	ASSERT_EQ(stream->prepareSegment(0, segment_ms, 250), VodEncodeStream::SegmentPrep::WaitColdStart);
	ASSERT_EQ(stream->liveHeadCount(), 1);
	ASSERT_EQ(stream->headWindowsForTest()[0].second, 100) << "sanity check on the window this test relies on";

	// The head's real output reaching exactly its own declared boundary is
	// the overrun signal tick() watches for.
	touchSegment(100);
	stream->tick(250);

	EXPECT_EQ(stream->liveHeadCount(), 0) << "the overrunning head must be stopped and removed immediately, not left to finish its full run";
}

TEST_F(VodEncodeStreamTest, TickDoesNotStopAHeadThatHasNotReachedItsWindowEnd)
{
	// Negative case for the above — tick() must not be trigger-happy and
	// stop a perfectly normal, still-within-window head.
	auto stream     = makeStream();
	auto segment_ms = segmentStartMs(250);
	ASSERT_EQ(stream->prepareSegment(0, segment_ms, 250), VodEncodeStream::SegmentPrep::WaitColdStart);

	touchSegment(50); // well short of window_end_segment(100)
	stream->tick(250);

	EXPECT_EQ(stream->liveHeadCount(), 1);
}

TEST_F(VodEncodeStreamTest, FastPathDiskHitRefreshesLastRequestedForLruEviction)
{
	// Regression test: a head whose segments are all being served straight
	// from the already-on-disk fast path (the common, intended case — it
	// raced ahead of the viewer) must still count as "recently requested"
	// for evictOneIfAtCap()'s LRU choice. Before the fix, last_requested/
	// last_requested_at_ms were only ever touched on the cache-*miss* path
	// below, so a head serving its entire backlog straight from disk looked
	// frozen at its spawn-time timestamp — indistinguishable from one nobody
	// wants anymore — the moment a third, unrelated request forced an
	// eviction at the live-head cap.
	VodEncodeStream::ArgsBuilder args = [](int, int64_t, std::optional<double>)
	{
		return std::vector<std::string>{"sleep", "30"};
	};
	VodEncodeStream stream("video", dir.string(), "seg-", args,
						   /*buffer_size=*/65536, /*ffmpeg_debug_logs=*/false,
						   /*verbose_transcode_logs=*/false,
						   /*lookahead_secs=*/1200, /*hls_time_secs=*/6); // huge window — never pauses in this test
	auto segment_ms = segmentStartMs(500);

	ASSERT_EQ(stream.prepareSegment(0, segment_ms, 500), VodEncodeStream::SegmentPrep::WaitColdStart);
	ASSERT_EQ(stream.liveHeadCount(), 1); // head A, [0,100)
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	ASSERT_EQ(stream.prepareSegment(150, segment_ms, 500), VodEncodeStream::SegmentPrep::WaitColdStart);
	ASSERT_EQ(stream.liveHeadCount(), 2); // head B, [150,250)
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	// Head A has been producing (and a viewer consuming) segment 0 the whole
	// time in between — simulate that purely through the fast, already-on-
	// disk path, exactly as normal playback would.
	touchSegment(0);
	ASSERT_EQ(stream.prepareSegment(0, segment_ms, 500), VodEncodeStream::SegmentPrep::Ready);

	// A third, unrelated request now forces an eviction at the live-head cap
	// (2). Head A was just used a moment ago via the fast path; head B has
	// not been touched since its own spawn — B must be the one evicted.
	ASSERT_EQ(stream.prepareSegment(300, segment_ms, 500), VodEncodeStream::SegmentPrep::WaitColdStart);
	ASSERT_EQ(stream.liveHeadCount(), 2);

	auto windows = stream.headWindowsForTest();
	std::sort(windows.begin(), windows.end());
	ASSERT_EQ(windows.size(), 2u);
	EXPECT_EQ(windows[0].first, 0) << "head A (recently used via the disk fast-path) must survive";
	EXPECT_EQ(windows[1].first, 300) << "the newly spawned head covering the actual request";
}

TEST_F(VodEncodeStreamTest, PausedHeadIsNotTreatedAsStalledEvenAfterALongIdlePeriod)
{
	// Regression test: pausing is this class's OWN deliberate throttle
	// (tick()'s pause/resume hysteresis, once a head built up a healthy
	// lead) — it is not the head being stuck. Before the fix, prepareSegment()'s
	// "stalled" heuristic couldn't tell the difference: any head idle past
	// stall_timeout_ms (for *any* reason) got only the tight catch-up margin,
	// so a normal viewer catching up to a paused head's backlog — which
	// routinely takes far longer than the stall timeout, since that's the
	// whole point of pre-buffering — got the head evicted and cold-respawned
	// instead of cheaply resumed.
	VodEncodeStream::ArgsBuilder args = [](int, int64_t, std::optional<double>)
	{
		return std::vector<std::string>{"sleep", "30"};
	};
	// lookahead_secs/hls_time_secs -> window_segments = 2/1 = 2, so the head
	// pauses quickly once it's got a small lead. stall_timeout_ms is tiny so
	// the test can wait past it without a real multi-second sleep.
	VodEncodeStream stream("video", dir.string(), "seg-", args,
						   /*buffer_size=*/65536, /*ffmpeg_debug_logs=*/false,
						   /*verbose_transcode_logs=*/false,
						   /*lookahead_secs=*/2, /*hls_time_secs=*/1,
						   /*head_window_segments=*/100, /*stall_timeout_ms=*/20);
	auto segment_ms = segmentStartMs(250);

	ASSERT_EQ(stream.prepareSegment(0, segment_ms, 250), VodEncodeStream::SegmentPrep::WaitColdStart);
	ASSERT_EQ(stream.liveHeadCount(), 1);

	// Simulate the head racing ahead and building up a healthy backlog.
	for (int i = 0; i <= 5; ++i) touchSegment(i);
	stream.tick(250);
	ASSERT_TRUE(stream.anyHeadPaused()) << "sanity check: the head should have paused once it built up a lead";

	// Wait past stall_timeout_ms with the head legitimately idle (paused) —
	// same as a viewer taking a while to watch through the backlog.
	std::this_thread::sleep_for(std::chrono::milliseconds(40));

	// The viewer catches up just past the paused frontier (generated=5):
	// within the generous "still working" margin (20) this should get, but
	// beyond the tight "genuinely stalled" one (4) the old, pause-blind
	// check would have wrongly applied here.
	auto result = stream.prepareSegment(12, segment_ms, 250);

	EXPECT_EQ(result, VodEncodeStream::SegmentPrep::WaitShort)
		<< "a paused-but-healthy head should be resumed and waited on, not evicted for a cold respawn";
	ASSERT_EQ(stream.liveHeadCount(), 1);
	EXPECT_EQ(stream.headWindowsForTest()[0].first, 0) << "must still be the original head, not a fresh one";
	EXPECT_FALSE(stream.anyHeadPaused()) << "prepareSegment() should have resumed it";
}