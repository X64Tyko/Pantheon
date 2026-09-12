#include <gtest/gtest.h>
#include "stream/PreviewSession.h"
#include "kairos/KairosClient.h"
#include "kairos/KairosTypes.h"
#include <filesystem>
#include <thread>

// PreviewSession's ffmpeg-spawning path (switchChannel()) requires a real
// Kairos backend (kairos.getNow()), a real ffprobe binary, and a real ffmpeg
// process — none of which are available in a unit-test environment without
// mocking infrastructure this codebase doesn't have yet (KairosClient isn't
// virtual/mockable — see test_discontinuity_sequence.cpp's identical
// "http://unused.invalid" construction-only pattern for ChannelSession).
// This file therefore covers everything that's genuinely separable from
// that: the pure computeOffset math (PreviewSession's own copy of
// ChannelSession::computeOffset), and the session's bookkeeping surface
// (dir()/sessionId()/isActive()/isIdle()/touch()/stop()) that never touches
// ffmpeg as long as switchChannel() itself is never called. In particular,
// dir() — the thing GuidePreview.tsx's stability comment and this class's
// own header comment are both about — is proven here to be a pure function
// of session_id alone, with no channel_id parameter anywhere it could even
// depend on one.

namespace
{
	class PreviewSessionTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			root = std::filesystem::temp_directory_path() /
				("momus_preview_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + testName());
			std::filesystem::create_directories(root);
			kairos = std::make_unique<KairosClient>("http://unused.invalid");
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

		std::unique_ptr<PreviewSession> makeSession(const std::string& sessionId, int lingerSecs = 60)
		{
			PreviewStreamOptions opts;
			opts.hls_root    = root.string();
			opts.linger_secs = lingerSecs;
			return std::make_unique<PreviewSession>(sessionId, "ffmpeg", opts, *kairos, /*hdr_capable=*/false);
		}

		std::filesystem::path root;
		std::unique_ptr<KairosClient> kairos;
	};
} // namespace

// ── computeOffset (pure math) ───────────────────────────────────────────────

TEST(PreviewSessionOffsetTest, NormalItemAtStart)
{
	KairosNowResponse item;
	item.wall_clock_start_ms = 1000;
	item.duration_ms         = 5000;
	item.is_filler           = false;
	EXPECT_EQ(PreviewSession::computeOffsetForTest(item, 1000), 0);
}

TEST(PreviewSessionOffsetTest, NormalItemPartway)
{
	KairosNowResponse item;
	item.wall_clock_start_ms = 1000;
	item.duration_ms         = 5000;
	item.is_filler           = false;
	EXPECT_EQ(PreviewSession::computeOffsetForTest(item, 3500), 2500);
}

TEST(PreviewSessionOffsetTest, ClampsToZeroBeforeStart)
{
	KairosNowResponse item;
	item.wall_clock_start_ms = 5000;
	item.duration_ms         = 5000;
	item.is_filler           = false;
	EXPECT_EQ(PreviewSession::computeOffsetForTest(item, 1000), 0);
}

TEST(PreviewSessionOffsetTest, FillerLoopsOnItsOwnDuration)
{
	KairosNowResponse item;
	item.wall_clock_start_ms = 1000;
	item.duration_ms         = 5000;
	item.is_filler           = true;

	EXPECT_EQ(PreviewSession::computeOffsetForTest(item, 3000), 2000); // first loop
	EXPECT_EQ(PreviewSession::computeOffsetForTest(item, 7000), 1000); // second loop
	EXPECT_EQ(PreviewSession::computeOffsetForTest(item, 11000), 0);   // third loop, exact boundary
}

TEST(PreviewSessionOffsetTest, FillerWithZeroDurationNeverDivides)
{
	// duration_ms > 0 guard — a filler with an unknown/zero duration must
	// fall back to the raw (non-looping) offset rather than mod-by-zero.
	KairosNowResponse item;
	item.wall_clock_start_ms = 1000;
	item.duration_ms         = 0;
	item.is_filler           = true;
	EXPECT_EQ(PreviewSession::computeOffsetForTest(item, 4000), 3000);
}

// ── dir() / manifest URL stability ──────────────────────────────────────────

TEST_F(PreviewSessionTest, DirIsDerivedOnlyFromSessionId)
{
	// switchChannel() takes a channel_id, but dir() — which Router.cpp's
	// /stream/preview/:id/playlist.m3u8 handler serves from, and which the
	// "manifest_url" returned to the client is built from — has no
	// channel-shaped input at all. This is *why* the manifest URL survives
	// channel switches: there's structurally nowhere for a channel_id to
	// enter the path.
	auto session = makeSession("abc123");
	EXPECT_EQ(session->dir(), (root / "preview" / "abc123").string());
}

TEST_F(PreviewSessionTest, DifferentSessionsGetDifferentDirs)
{
	auto a = makeSession("session-a");
	auto b = makeSession("session-b");
	EXPECT_NE(a->dir(), b->dir());
	EXPECT_EQ(a->sessionId(), "session-a");
	EXPECT_EQ(b->sessionId(), "session-b");
}

// ── lifecycle bookkeeping (no ffmpeg ever spawned) ──────────────────────────

TEST_F(PreviewSessionTest, StartsInactive)
{
	auto session = makeSession("s1");
	EXPECT_FALSE(session->isActive());
}

TEST_F(PreviewSessionTest, NeverTouchedIsNeverIdle)
{
	// last_touch_ms == 0 (the "never touched" sentinel) is explicitly
	// special-cased to false, not "infinitely idle" — a session that never
	// got as far as touch() (e.g. switchChannel() failed before reaching it)
	// shouldn't look reapable purely from having a zero timestamp.
	auto session = makeSession("s1", /*lingerSecs=*/0);
	EXPECT_FALSE(session->isIdle());
}

TEST_F(PreviewSessionTest, IdleImmediatelyAfterTouchWithZeroLinger)
{
	auto session = makeSession("s1", /*lingerSecs=*/0);
	session->touch();
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	EXPECT_TRUE(session->isIdle());
}

TEST_F(PreviewSessionTest, NotIdleRightAfterTouchWithGenerousLinger)
{
	auto session = makeSession("s1", /*lingerSecs=*/3600);
	session->touch();
	EXPECT_FALSE(session->isIdle());
}

TEST_F(PreviewSessionTest, StopOnNeverStartedSessionIsSafeNoOp)
{
	// stop() must be safe to call on a session whose switchChannel() never
	// ran (active still false, ffmpeg still null) — this is exactly the path
	// the destructor takes for a session PreviewSessionManager::create()
	// discards after a failed switchChannel() (see PreviewSessionManagerTest
	// below), and it must not touch the null ffmpeg unique_ptr.
	auto session = makeSession("s1");
	session->stop();
	session->stop(); // idempotent
	EXPECT_FALSE(session->isActive());
}

TEST_F(PreviewSessionTest, DestructorOnNeverStartedSessionIsSafe)
{
	auto session = makeSession("s1");
	session.reset(); // ~PreviewSession() -> stop(); must not crash
	SUCCEED();
}