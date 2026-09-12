#include <gtest/gtest.h>
#include "stream/PreviewSessionManager.h"
#include "kairos/KairosClient.h"
#include <filesystem>

// PreviewSessionManager::create() is real, end-to-end code: it calls
// PreviewSession::switchChannel(), which calls kairos.getNow(channel_id)
// over real HTTP. Pointed at "http://unused.invalid" (same construction-only
// stand-in test_discontinuity_sequence.cpp/test_channel_session.cpp already
// use for KairosClient), that call fails fast and getNow() resolves to
// nullopt — which switchChannel() treats as "channel offline," falling back
// to opts.default_logo_path. Left empty (as it is below), that path is a
// documented failure case with no ffmpeg involved at all: switchChannel()
// logs an error and returns false *before* ever building an ffmpeg argv.
// That failure path is exactly what's exercised here — it's the one place
// create()'s full real logic (mkdir, kairos round-trip, session bookkeeping)
// is reachable without a real ffmpeg binary or a real Kairos server. Because
// it fails, `session_opts.default_logo_path` and a real spawn are never
// reached — this file cannot and does not test a successful create()
// (an actual ffmpeg spawn — offline slate or real content), reaping-thread
// timing (5s poll interval, real background threads), or the
// verbose_transcode_logs/buffer_size settings-cache refresh (also gated on a
// reachable Kairos). Those would need real ffmpeg + a fake/mock Kairos HTTP
// server, which don't exist in this test harness yet.

namespace
{
	class PreviewSessionManagerTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			root = std::filesystem::temp_directory_path() /
				("momus_previewmgr_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + testName());
			std::filesystem::create_directories(root);
			kairos = std::make_unique<KairosClient>("http://unused.invalid");

			opts.hls_root = root.string();
			// default_logo_path deliberately left empty — see file comment.
			manager = std::make_unique<PreviewSessionManager>("ffmpeg", opts, *kairos);
		}

		void TearDown() override
		{
			manager.reset();
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		std::string testName() const
		{
			return ::testing::UnitTest::GetInstance()->current_test_info()->name();
		}

		std::filesystem::path root;
		std::unique_ptr<KairosClient> kairos;
		PreviewStreamOptions opts;
		std::unique_ptr<PreviewSessionManager> manager;
	};
} // namespace

TEST_F(PreviewSessionManagerTest, ConstructionDoesNotCrashWithUnreachableKairos)
{
	// Constructor blocks on one synchronous refreshSettings() call — must
	// fail gracefully (KairosClient's getBufferSize/getVerboseTranscodeLogs
	// are documented to return nullopt on any failure) rather than hang or
	// throw. Reaching TearDown at all is the assertion.
	SUCCEED();
}

TEST_F(PreviewSessionManagerTest, GetOnEmptyManagerReturnsNull)
{
	EXPECT_EQ(manager->get("nonexistent"), nullptr);
}

TEST_F(PreviewSessionManagerTest, CreateFailsGracefullyWithNoOfflineImageConfigured)
{
	// channel_id is never even resolved (kairos is unreachable) — the
	// "offline, no logo configured" branch inside switchChannel() is what
	// actually fails this, deterministically, with no ffmpeg spawn.
	auto session = manager->create("some-channel", /*hdr_capable=*/false);
	EXPECT_EQ(session, nullptr);
}

TEST_F(PreviewSessionManagerTest, FailedCreateNeverRegistersASession)
{
	auto session = manager->create("some-channel", false);
	ASSERT_EQ(session, nullptr);
	// create() only inserts into `sessions` after a successful
	// switchChannel() — a failed one must leave nothing behind for get() to
	// find under any id, including whatever id was internally generated.
	EXPECT_EQ(manager->get("some-channel"), nullptr);
}

TEST_F(PreviewSessionManagerTest, SwitchChannelOnUnknownSessionReturnsFalse)
{
	EXPECT_FALSE(manager->switchChannel("nonexistent-session", "some-channel"));
}

TEST_F(PreviewSessionManagerTest, StopOnUnknownSessionIsSafeNoOp)
{
	manager->stop("nonexistent-session");
	SUCCEED();
}

TEST_F(PreviewSessionManagerTest, RepeatedFailedCreatesAreIndependentAndSafe)
{
	// Nothing shared/leaked between two independent failed create() calls
	// (e.g. no stale ffmpeg_mtx lock, no reused session_id causing a false
	// "already exists" collision).
	auto a = manager->create("channel-a", false);
	auto b = manager->create("channel-b", true);
	EXPECT_EQ(a, nullptr);
	EXPECT_EQ(b, nullptr);
}