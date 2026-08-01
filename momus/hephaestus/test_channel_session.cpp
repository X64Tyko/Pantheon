#include <gtest/gtest.h>
#include "stream/ChannelSession.h"
#include "kairos/KairosTypes.h"
#include "kairos/KairosClient.h"

#include <cstdlib>
#include <filesystem>

TEST(ChannelSessionTest, ComputeOffset)
{
	KairosNowResponse item;
	item.wall_clock_start_ms = 1000;
	item.duration_ms         = 5000;
	item.is_filler           = false;

	// Normal item: at start
	EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 1000), 0);
	// Normal item: 2s in
	EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 3000), 2000);
	// Normal item: behind start (should clamp to 0 per impl)
	EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 500), 0);
}

TEST(ChannelSessionTest, ComputeOffsetFiller)
{
	KairosNowResponse item;
	item.wall_clock_start_ms = 1000;
	item.duration_ms         = 5000;
	item.is_filler           = true;

	// Filler: first loop
	EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 3000), 2000);
	// Filler: second loop
	EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 7000), 1000);
	// Filler: third loop
	EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 11000), 0);
}

TEST(ChannelSessionTest, ComputeSpeed)
{
	// 100s duration
	int64_t duration = 100000;

	// Running 1s behind schedule (drift = +1000ms)
	// speed = 100000 / (100000 - 1000) = 100000 / 99000 = 1.0101...
	auto s1 = ChannelSession::computeSpeedForTest(1000, duration);
	ASSERT_TRUE(s1.has_value());
	EXPECT_NEAR(*s1, 1.0101, 0.0001);

	// Running 1s ahead (drift = -1000ms)
	// speed = 100000 / (100000 - (-1000)) = 100000 / 101000 = 0.99009...
	auto s2 = ChannelSession::computeSpeedForTest(-1000, duration);
	ASSERT_TRUE(s2.has_value());
	EXPECT_NEAR(*s2, 0.9901, 0.0001);

	// Too much drift (10s behind on 100s duration)
	// speed = 100000 / 90000 = 1.111... (Max is likely 1.02)
	auto s3 = ChannelSession::computeSpeedForTest(10000, duration);
	EXPECT_FALSE(s3.has_value());

	// Zero drift
	auto s4 = ChannelSession::computeSpeedForTest(0, duration);
	EXPECT_FALSE(s4.has_value());
}

// ── start()'s retry-on-unreachable-Kairos fix ───────────────────────────────
//
// Before this session's fix, a session whose very first /now lookup failed
// (Kairos down/restarting right as a viewer connects) got stuck showing the
// unbounded cold-start splash forever — in_splash stayed true and nothing
// was ever scheduled to retry. The fix makes applyResolvedItem()'s failure
// branch behave like transition()'s own "Kairos has nothing at all" fallback:
// synthesize a bounded offline item, leave in_splash false, and spawn it —
// so onExit() naturally chains back into transition() (and so back into a
// fresh /now attempt) once the bounded encode finishes, instead of landing
// in onExit()'s in_splash branch, which just respawns the same unbounded
// splash forever.
//
// Unlike PreviewSessionManagerTest's equivalent scenario (see that file's own
// comment — it deliberately leaves default_logo_path empty so no real ffmpeg
// spawn is ever reached), this test needs the session to stay genuinely
// active with a real splash actually running: the whole point is proving
// onExit()->transition() has something live to chain through, which only
// happens past spawnOffline()'s own "no image configured, give up" branch.
// So this generates a tiny real fixture image and lets a real (bundled
// ffmpeg) offline-slate process spawn — the one Hephaestus test file that
// does, deliberately, for exactly this reason.
namespace
{
	class ChannelSessionRetryTest : public ::testing::Test
	{
	protected:
		std::filesystem::path root;
		std::filesystem::path logo;
		std::unique_ptr<KairosClient> kairos;

		void SetUp() override
		{
			root = std::filesystem::temp_directory_path() /
				("momus_channel_retry_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
			std::filesystem::create_directories(root);
			logo = root / "logo.png";

			std::string cmd = "ffmpeg -y -v quiet -f lavfi -i color=c=red:s=2x2 -frames:v 1 '" + logo.string() + "'";
			std::system(cmd.c_str());

			// Unreachable, not just erroring — DNS resolution failure fails
			// fast (same "http://unused.invalid" pattern test_preview_session.cpp/
			// test_preview_session_manager.cpp already establish), reliably
			// landing start() on its synchronous fast path (the lookup
			// resolves to nullopt well within kFastPathBudget), which is what
			// exercises applyResolvedItem()'s fix directly rather than going
			// through the separate async dispatch path around it.
			kairos = std::make_unique<KairosClient>("http://unused.invalid");
		}

		void TearDown() override
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		std::shared_ptr<ChannelSession> makeSession()
		{
			StreamOptions opts;
			opts.default_logo_path = logo.string();
			return std::make_shared<ChannelSession>("chan-1", *kairos, "ffmpeg", opts);
		}
	};
} // namespace

TEST_F(ChannelSessionRetryTest, UnreachableKairosAtStartEntersBoundedRetryNotStuckSplash)
{
	ASSERT_TRUE(std::filesystem::exists(logo)) << "fixture image generation failed — is ffmpeg on PATH?";

	auto session = makeSession();
	session->start();

	EXPECT_TRUE(session->isActive());
	EXPECT_FALSE(session->inSplashForTest())
		<< "must have left the unbounded cold-start splash, or onExit() can never chain into a retry";
	EXPECT_EQ(session->currentItemTypeForTest(), "offline");
	EXPECT_GT(session->currentItemWallClockEndMsForTest(), 0)
		<< "must be a bounded fallback item (real wall_clock_end_ms), not the unbounded default-constructed splash sentinel";

	session->stop();
}