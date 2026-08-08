// End-to-end VOD playback tests — unlike test_vod_encode_stream.cpp (a fake
// "sleep" process standing in for ffmpeg), these drive the real thing: a
// real VodSessionManager, a real local fixture file, and real ffmpeg
// direct-stream/copy encodes, exactly as Router.cpp does for an actual
// player. This is possible without a mock Kairos server the way a
// ChannelSession/PreviewSession-level test would need one (see
// test_preview_session_manager.cpp's own comment on why that file can't do
// this): VodSessionManager::create() takes file_path as a direct parameter
// — Kairos resolves it in the real request path (Router.cpp), but the
// manager itself never touches Kairos to do so. The one remaining Kairos
// dependency (refreshSettings()'s verbose_transcode_logs/buffer_size poll,
// blocking once at construction) already tolerates an unreachable server —
// same "http://unused.invalid" stand-in every other manager-level test here
// uses.
//
// The fixture (vod_playback_sample.mp4) is a synthetic, license-free 30s
// clip with an 8-second keyframe interval — deliberately SPARSER than
// kVodHlsSegmentSecs (6s), so a direct-stream video's real per-segment cuts
// land at 8/16/24s, not a uniform 6s grid. PlaysThroughEveryVideoAndAudio-
// SegmentWithMatchingRealDurations is a direct regression test for the bug
// this fixture exists to catch: video and audio are two independent ffmpeg
// processes sharing ONE #EXTINF list (built from video's own real
// boundaries — see VodSession::buildStaticPlaylist()'s comment), so audio's
// own real cuts have to be pinned to those same boundaries rather than
// following its own separate, uniform cadence — see buildVodAudioArgs()'s
// comment for the full story. A source file with keyframes exactly on the
// nominal 6s grid would never have caught this: audio's independent
// -hls_time cut would have coincidentally matched anyway.

#include <gtest/gtest.h>
#include "stream/VodSessionManager.h"
#include "kairos/KairosClient.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
	std::string fixturePath() { return std::string(MOMUS_HEPHAESTUS_FIXTURES_DIR) + "/vod_playback_sample.mp4"; }

	// Mirrors Router.cpp's own waitForFile() — real ffmpeg processes here (not
	// VodEncodeStreamTest's fake "sleep" stand-in), so segment production
	// genuinely takes real, if short, wall-clock time.
	bool waitForFile(const std::string& path, int maxWaitMs = 20000)
	{
		for (int waited = 0; waited < maxWaitMs; waited += 50)
		{
			if (std::filesystem::exists(path)) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return std::filesystem::exists(path);
	}

	struct PlaylistEntry
	{
		double duration;
		std::string filename;
	};

	// Minimal #EXTINF/filename pair extraction — every non-'#' line in these
	// playlists (buildStaticPlaylist()'s own output) is a segment filename,
	// each preceded by exactly one #EXTINF line, same assumption Router.cpp's
	// own rewriteChannelViewerPlaylist() makes about ffmpeg-muxer-shaped M3U8.
	std::vector<PlaylistEntry> parsePlaylist(const std::string& path)
	{
		std::vector<PlaylistEntry> out;
		std::ifstream f(path);
		std::string line;
		double pending = -1;
		while (std::getline(f, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.rfind("#EXTINF:", 0) == 0) pending = std::stod(line.substr(8));
			else if (!line.empty() && line[0] != '#')
			{
				out.push_back({pending, line});
				pending = -1;
			}
		}
		return out;
	}

	// Real wall-clock duration of a .ts file's own media content, via a real
	// ffprobe invocation — the whole point of this file is checking that
	// reality (what ffmpeg actually wrote) matches the manifest (what
	// Hephaestus told the player to expect).
	double ffprobeDurationSecs(const std::string& path)
	{
		std::string cmd =
			"ffprobe -v error -show_entries format=duration -of default=nokey=1:noprint_wrappers=1 \"" + path + "\"";
		FILE* pipe = popen(cmd.c_str(), "r");
		if (!pipe) return -1.0;
		std::string result;
		char buf[256];
		while (fgets(buf, sizeof(buf), pipe)) result += buf;
		pclose(pipe);
		try { return std::stod(result); }
		catch (...) { return -1.0; }
	}

	class VodSessionPlaybackTest : public ::testing::Test
	{
	protected:
		std::filesystem::path root;
		std::unique_ptr<KairosClient> kairos;
		VodStreamOptions opts;
		std::unique_ptr<VodSessionManager> manager;

		void SetUp() override
		{
			root = std::filesystem::temp_directory_path() /
				("momus_vod_playback_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + testName());
			std::filesystem::create_directories(root);
			// Same unreachable-Kairos stand-in as test_preview_session_manager.cpp —
			// refreshSettings()'s one blocking call at construction tolerates it,
			// and nothing else here needs a real Kairos (file_path is a direct
			// create() parameter).
			kairos        = std::make_unique<KairosClient>("http://unused.invalid");
			opts.hls_root = root.string();
			manager       = std::make_unique<VodSessionManager>("ffmpeg", opts, *kairos);
		}

		void TearDown() override
		{
			manager.reset();
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		std::string testName() const { return ::testing::UnitTest::GetInstance()->current_test_info()->name(); }

		std::shared_ptr<VodSession> startSession(const std::string& content_id, int64_t position_ms = 0)
		{
			return manager->create("movie", content_id, fixturePath(), position_ms,
								   /*audio_track=*/-1, /*subtitle_track=*/-1, /*hdr_capable=*/false,
								   /*client_caps=*/std::nullopt, /*external_subtitles=*/{},
								   /*fallback_duration_ms=*/0);
		}
	};
} // namespace

TEST_F(VodSessionPlaybackTest, PlaysThroughEveryVideoAndAudioSegmentWithMatchingRealDurations)
{
	auto session = startSession("content-full-playthrough");
	ASSERT_NE(session, nullptr);
	ASSERT_TRUE(session->directStream()) << "fixture is plain h264/aac — should always resolve to direct-stream";

	ASSERT_TRUE(waitForFile(session->dir() + "/playlist.m3u8"));
	ASSERT_TRUE(waitForFile(session->dir() + "/audio-playlist.m3u8"));
	auto videoEntries = parsePlaylist(session->dir() + "/playlist.m3u8");
	auto audioEntries = parsePlaylist(session->dir() + "/audio-playlist.m3u8");
	ASSERT_FALSE(videoEntries.empty());
	ASSERT_EQ(videoEntries.size(), audioEntries.size()) << "one shared segment count/timeline for both streams";

	// Generous but far tighter than the ~4s mismatch this test would have
	// caught before the fix — real AAC-frame/keyframe quantization only ever
	// accounts for a fraction of a second either way.
	constexpr double kToleranceSecs = 0.5;

	for (size_t i = 0; i < videoEntries.size(); ++i)
	{
		int idx = static_cast<int>(i);

		ASSERT_NE(session->prepareSegment(idx), VodSession::SegmentPrep::Failed);
		ASSERT_TRUE(waitForFile(session->videoSegmentFilePath(idx))) << "video segment " << i << " never appeared";
		double actualVideo = ffprobeDurationSecs(session->videoSegmentFilePath(idx));
		EXPECT_NEAR(actualVideo, videoEntries[i].duration, kToleranceSecs)
			<< "video segment " << i << " real duration doesn't match its own playlist's #EXTINF";

		ASSERT_NE(session->prepareAudioSegment(idx), VodSession::SegmentPrep::Failed);
		ASSERT_TRUE(waitForFile(session->audioSegmentFilePath(idx))) << "audio segment " << i << " never appeared";
		double actualAudio = ffprobeDurationSecs(session->audioSegmentFilePath(idx));
		EXPECT_NEAR(actualAudio, audioEntries[i].duration, kToleranceSecs)
			<< "audio segment " << i << " real duration doesn't match the SAME shared #EXTINF video's real "
			"keyframe-driven cuts predicted — this is the direct-stream regression this file exists to catch";
	}
}

TEST_F(VodSessionPlaybackTest, MultiplePlayheadsOnSameContentShareTheStreamWithoutCorruptingEachOther)
{
	// Two viewers of the same content, starting at different positions —
	// the "multiple playheads" case: both must land on the identical shared
	// VodEncodeStream (VodSessionManager.h's own class comment) and be able
	// to play independently without one corrupting or evicting the other's
	// already-fetched segments.
	auto sessionA = startSession("content-shared-playheads", /*position_ms=*/0);
	ASSERT_NE(sessionA, nullptr);
	auto sessionB = startSession("content-shared-playheads", /*position_ms=*/16000);
	ASSERT_NE(sessionB, nullptr);
	EXPECT_NE(sessionA->sessionId(), sessionB->sessionId());

	// Same content -> same shared stream -> identical on-disk segment path
	// for a given index regardless of which viewer's facade asks for it.
	EXPECT_EQ(sessionA->videoSegmentFilePath(0), sessionB->videoSegmentFilePath(0));

	ASSERT_NE(sessionA->prepareSegment(0), VodSession::SegmentPrep::Failed);
	ASSERT_TRUE(waitForFile(sessionA->videoSegmentFilePath(0)));
	double firstDurationSeenByA = ffprobeDurationSecs(sessionA->videoSegmentFilePath(0));
	EXPECT_GT(firstDurationSeenByA, 0);

	// B plays from further into the file — segment 2 (its own start_segment
	// landed there per position_ms=16000) must resolve correctly and
	// independently of A's concurrent activity on segment 0 of the same
	// shared stream.
	ASSERT_NE(sessionB->prepareSegment(2), VodSession::SegmentPrep::Failed);
	ASSERT_TRUE(waitForFile(sessionB->videoSegmentFilePath(2)));
	EXPECT_GT(ffprobeDurationSecs(sessionB->videoSegmentFilePath(2)), 0);

	// A's own earlier segment must still be exactly what it was — B's
	// activity elsewhere in the shared stream must not have evicted or
	// overwritten it.
	ASSERT_TRUE(std::filesystem::exists(sessionA->videoSegmentFilePath(0)));
	EXPECT_NEAR(ffprobeDurationSecs(sessionA->videoSegmentFilePath(0)), firstDurationSeenByA, 0.01);
}

TEST_F(VodSessionPlaybackTest, EpisodeAdvancementStartsACleanSessionAfterThePriorOneStops)
{
	// "Episode advancement": stop the current item's session, start the
	// next one — must not leave any state (paused/live heads, directories,
	// manager bookkeeping) that corrupts or blocks the new session.
	auto sessionA = startSession("content-episode-1");
	ASSERT_NE(sessionA, nullptr);
	ASSERT_NE(sessionA->prepareSegment(0), VodSession::SegmentPrep::Failed);
	ASSERT_TRUE(waitForFile(sessionA->videoSegmentFilePath(0)));

	std::string sessionAId = sessionA->sessionId();
	sessionA.reset(); // drop this test's own reference, same as a Router handler letting its local shared_ptr go
	manager->stop(sessionAId);
	EXPECT_EQ(manager->get(sessionAId), nullptr);

	auto sessionB = startSession("content-episode-2");
	ASSERT_NE(sessionB, nullptr);
	EXPECT_NE(sessionB->sessionId(), sessionAId);
	ASSERT_NE(sessionB->prepareSegment(0), VodSession::SegmentPrep::Failed);
	ASSERT_TRUE(waitForFile(sessionB->videoSegmentFilePath(0)));
	EXPECT_GT(ffprobeDurationSecs(sessionB->videoSegmentFilePath(0)), 0);
}