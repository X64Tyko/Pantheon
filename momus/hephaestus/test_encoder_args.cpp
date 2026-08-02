#include <gtest/gtest.h>
#include "stream/EncoderArgs.h"
#include <vector>
#include <string>
#include <algorithm>

TEST(EncoderArgsTest, DecodeCodecKey)
{
	EXPECT_EQ(decodeCodecKey("h264", 8), "h264");
	EXPECT_EQ(decodeCodecKey("h264", 10), "h264"); // h264 ignores bit depth
	EXPECT_EQ(decodeCodecKey("hevc", 8), "hevc");
	EXPECT_EQ(decodeCodecKey("hevc", 10), "hevc10");
	EXPECT_EQ(decodeCodecKey("hevc", 12), "hevc12");
	EXPECT_EQ(decodeCodecKey("av1", 8), "av1");
	EXPECT_EQ(decodeCodecKey("av1", 10), "av110");
}

TEST(EncoderArgsTest, ResolveMaxHeight)
{
	EXPECT_EQ(resolveMaxHeight("1080p"), 1080);
	EXPECT_EQ(resolveMaxHeight("720p"), 720);
	EXPECT_EQ(resolveMaxHeight("480p"), 480);
	EXPECT_EQ(resolveMaxHeight("source"), 0);
	EXPECT_EQ(resolveMaxHeight("unknown"), 0);
	EXPECT_EQ(resolveMaxHeight(""), 0);
}

TEST(EncoderArgsTest, PushScaleFilter)
{
	std::vector<std::string> vf;

	// maxHeight 0 -> no-op
	pushScaleFilter(vf, 0);
	EXPECT_TRUE(vf.empty());

	// maxHeight 720
	pushScaleFilter(vf, 720);
	ASSERT_EQ(vf.size(), 1);
	// The implementation escapes commas for use in -vf chains
	EXPECT_EQ(vf[0], "scale=-2:min(ih\\,720)");
}

TEST(EncoderArgsTest, PushBitrateCapArgs)
{
	std::vector<std::string> a;

	// 0 -> no-op
	pushBitrateCapArgs(a, 0);
	EXPECT_TRUE(a.empty());

	// 2000 kbps
	pushBitrateCapArgs(a, 2000);
	ASSERT_EQ(a.size(), 4);
	EXPECT_EQ(a[0], "-maxrate");
	EXPECT_EQ(a[1], "2000k");
	EXPECT_EQ(a[2], "-bufsize");
	EXPECT_EQ(a[3], "4000k");
}

TEST(EncoderArgsTest, PushLogLevelArgs)
{
	std::vector<std::string> a;

	// false -> no-op
	pushLogLevelArgs(a, false);
	EXPECT_TRUE(a.empty());

	// true -> -v verbose, plus periodic -stats (ffmpeg only prints -stats
	// automatically on a tty, never true here since stderr is always piped
	// to FfmpegProcess — see pushLogLevelArgs' own comment)
	pushLogLevelArgs(a, true);
	ASSERT_EQ(a.size(), 5);
	EXPECT_EQ(a[0], "-v");
	EXPECT_EQ(a[1], "verbose");
	EXPECT_EQ(a[2], "-stats");
	EXPECT_EQ(a[3], "-stats_period");
	EXPECT_EQ(a[4], "2");
}

// ashowinfo pairs with the `showinfo` video filter a caller pushes into its
// own vfParts (ChannelSession.cpp's live-channel transcode branch) — together
// they give a per-stream pts_time trail for diagnosing A/V drift under
// verbose_transcode_logs. Off by default: too chatty (one line per audio
// frame) to leave on outside active diagnosis.
TEST(EncoderArgsTest, PushAudioEncoderArgs_DebugShowinfoAppendsAshowinfoFilter)
{
	// Off (default): no -af at all when nothing else needs the filter graph.
	{
		std::vector<std::string> a;
		pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, 192);
		EXPECT_EQ(std::find(a.begin(), a.end(), "-af"), a.end());
	}
	// On, nothing else in the filter chain: -af ashowinfo appears on its own.
	{
		std::vector<std::string> a;
		pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, 192,
							 std::nullopt, nullptr, /*debug_showinfo=*/true);
		auto it = std::find(a.begin(), a.end(), "-af");
		ASSERT_NE(it, a.end());
		ASSERT_NE(it + 1, a.end());
		EXPECT_EQ(*(it + 1), "ashowinfo");
	}
	// On alongside loudnorm: appended last, after dynaudnorm.
	{
		std::vector<std::string> a;
		pushAudioEncoderArgs(a, /*loudnorm=*/true, /*speed=*/1.0, 192,
							 std::nullopt, nullptr, /*debug_showinfo=*/true);
		auto it = std::find(a.begin(), a.end(), "-af");
		ASSERT_NE(it, a.end());
		ASSERT_NE(it + 1, a.end());
		const std::string& af = *(it + 1);
		EXPECT_NE(af.find("dynaudnorm"), std::string::npos);
		EXPECT_EQ(af.substr(af.size() - std::string(",ashowinfo").size()), ",ashowinfo");
	}
}

TEST(EncoderArgsTest, PushVaapiDeviceArg)
{
	std::vector<std::string> a;

	// No-op if not AMD
	pushVaapiDeviceArg(a, HwAccel::nvidia, HwAccel::none, "/dev/dri/renderD128");
	EXPECT_TRUE(a.empty());

	// Insert if AMD encode
	pushVaapiDeviceArg(a, HwAccel::amd, HwAccel::none, "/dev/dri/renderD128");
	ASSERT_EQ(a.size(), 2);
	EXPECT_EQ(a[0], "-vaapi_device");
	EXPECT_EQ(a[1], "/dev/dri/renderD128");

	a.clear();
	// Insert if AMD decode
	pushVaapiDeviceArg(a, HwAccel::none, HwAccel::amd, "/dev/dri/renderD129");
	ASSERT_EQ(a.size(), 2);
	EXPECT_EQ(a[1], "/dev/dri/renderD129");
}

TEST(EncoderArgsTest, PushHwAccelDecodeArgs)
{
	std::vector<std::string> a;
	std::set<std::string> decodable = {"h264", "hevc10"};

	// Match nvidia h264
	pushHwAccelDecodeArgs(a, HwAccel::nvidia, decodable, "h264");
	ASSERT_EQ(a.size(), 2);
	EXPECT_EQ(a[0], "-hwaccel");
	EXPECT_EQ(a[1], "cuda");

	a.clear();
	// Match amd hevc10
	pushHwAccelDecodeArgs(a, HwAccel::amd, decodable, "hevc10");
	ASSERT_EQ(a.size(), 2);
	EXPECT_EQ(a[1], "vaapi");

	a.clear();
	// No match: unknown codec
	pushHwAccelDecodeArgs(a, HwAccel::nvidia, decodable, "vp9");
	EXPECT_TRUE(a.empty());

	a.clear();
	// No match: codec not in decodable set
	pushHwAccelDecodeArgs(a, HwAccel::nvidia, decodable, "hevc");
	EXPECT_TRUE(a.empty());
}

TEST(EncoderArgsTest, PushVideoEncoderArgs)
{
	std::vector<std::string> a;
	std::vector<std::string> vf;

	// Software (none)
	pushVideoEncoderArgs(a, vf, HwAccel::none, 2);
	EXPECT_EQ(a[0], "-c:v");
	EXPECT_EQ(a[1], "libx264");
	// Verify keyframe interval
	auto it = std::find(a.begin(), a.end(), "-force_key_frames");
	ASSERT_NE(it, a.end());
	EXPECT_EQ(*(it + 1), "expr:gte(t,n_forced*2)");

	a.clear();
	vf.clear();
	// NVIDIA
	pushVideoEncoderArgs(a, vf, HwAccel::nvidia, 2);
	EXPECT_EQ(a[0], "-c:v");
	EXPECT_EQ(a[1], "h264_nvenc");

	a.clear();
	vf.clear();
	// AMD
	pushVideoEncoderArgs(a, vf, HwAccel::amd, 2);
	EXPECT_EQ(a[0], "-c:v");
	EXPECT_EQ(a[1], "h264_vaapi");
	// AMD needs format=nv12|vaapi upload
	ASSERT_FALSE(vf.empty());
}

// Regression coverage for the scene-cut-disable fix: NVENC's own scene-cut
// heuristic isn't disabled by -force_key_frames, so a real scene change
// between two forced keyframes could still insert an extra, unplanned one —
// irregular GOP structure the segmenter isn't expecting. -sc_threshold is
// confirmed silently ignored by h264_nvenc/hevc_nvenc; -no-scenecut is the
// flag that actually works there. libx264/libx265 use -sc_threshold/
// x265-params scenecut correctly instead.
TEST(EncoderArgsTest, PushVideoEncoderArgs_DisablesEncoderOwnSceneCutDetection)
{
	std::vector<std::string> a;
	std::vector<std::string> vf;

	pushVideoEncoderArgs(a, vf, HwAccel::nvidia, 2);
	auto it = std::find(a.begin(), a.end(), "-no-scenecut");
	ASSERT_NE(it, a.end()) << "NVENC ignores -sc_threshold; -no-scenecut is the flag that works";
	EXPECT_EQ(*(it + 1), "1");
	EXPECT_EQ(std::find(a.begin(), a.end(), "-sc_threshold"), a.end())
		<< "-sc_threshold is a silent no-op on NVENC — shouldn't be relied on there";

	a.clear();
	vf.clear();
	pushVideoEncoderArgs(a, vf, HwAccel::none, 2);
	auto it2 = std::find(a.begin(), a.end(), "-sc_threshold");
	ASSERT_NE(it2, a.end()) << "libx264 genuinely respects -sc_threshold, unlike NVENC";
	EXPECT_EQ(*(it2 + 1), "0");
}

// Regression coverage for the default bitrate cap: leaving CQ/VBR genuinely
// uncapped (channel.stream_video_bitrate == 0, the default) is a documented
// live-streaming anti-pattern — a real-time (-re-paced) pipeline has no
// slack to absorb an unbounded I-frame bitrate spike. defaultBitrateCapKbps
// must still scale with resolution (a 4K cap that's fine for 4K would be a
// real quality ceiling at 480p, and vice versa a 480p-sized cap would visibly
// constrain 4K).
TEST(EncoderArgsTest, DefaultBitrateCapKbps_ScalesWithResolution)
{
	EXPECT_EQ(defaultBitrateCapKbps(0), 8000) << "unknown height assumes ~1080p-ish";
	EXPECT_EQ(defaultBitrateCapKbps(480), 4000);
	EXPECT_EQ(defaultBitrateCapKbps(720), 6000);
	EXPECT_EQ(defaultBitrateCapKbps(1080), 10000);
	EXPECT_EQ(defaultBitrateCapKbps(2160), 20000); // 4K
	// Monotonically non-decreasing — a cap that got smaller at a higher
	// resolution would be a real bug (the whole point is to bound headroom,
	// not to visibly constrain quality).
	EXPECT_LE(defaultBitrateCapKbps(480), defaultBitrateCapKbps(720));
	EXPECT_LE(defaultBitrateCapKbps(720), defaultBitrateCapKbps(1080));
	EXPECT_LE(defaultBitrateCapKbps(1080), defaultBitrateCapKbps(2160));
}

TEST(EncoderArgsTest, EffectiveOutputHeight_SmallerOfConfiguredCapAndSourceHeight)
{
	VideoTrack source_1080p;
	source_1080p.height = 1080;
	VideoTrack source_4k;
	source_4k.height = 2160;

	// No configured cap ("source") — real output is the source's own height.
	EXPECT_EQ(effectiveOutputHeight(0, &source_1080p), 1080);
	// Configured cap above the source's own height — never upscales, so the
	// source's real height still governs.
	EXPECT_EQ(effectiveOutputHeight(1080, &source_4k), 1080);
	// Configured cap below the source's own height — the cap governs.
	EXPECT_EQ(effectiveOutputHeight(720, &source_4k), 720);
	// No source_video probed at all — falls back to whatever cap is
	// configured, or 0 (unknown) if there isn't one either.
	EXPECT_EQ(effectiveOutputHeight(720, nullptr), 720);
	EXPECT_EQ(effectiveOutputHeight(0, nullptr), 0);
}

// -g regression coverage: a hardware encoder (verified on NVENC) left to plan
// its own default GOP length instead of being told the real forced-keyframe
// cadence can stall/replan every time -force_key_frames actually fires —
// periodic, synced to the interval, and confirmed live by bisecting
// kLiveHlsSegmentSecs (2s reproduced a repeating stutter, 6s didn't, 2s
// reproduced it again). -g must match keyframeIntervalSecs in *frames*,
// derived from the source's own real frame rate, whenever that's knowable.
TEST(EncoderArgsTest, PushVideoEncoderArgs_GopSizeMatchesForcedKeyframeInterval)
{
	std::vector<std::string> a;
	std::vector<std::string> vf;
	VideoTrack source;
	source.r_frame_rate = "24000/1001"; // 23.976fps

	pushVideoEncoderArgs(a, vf, HwAccel::nvidia, 2, &source);

	auto it = std::find(a.begin(), a.end(), "-g");
	ASSERT_NE(it, a.end()) << "a determinable source frame rate must produce an explicit -g";
	// round(23.976... * 2) == 48
	EXPECT_EQ(*(it + 1), "48");
}

TEST(EncoderArgsTest, PushVideoEncoderArgs_NoGopSizeWithoutADeterminableFrameRate)
{
	std::vector<std::string> a;
	std::vector<std::string> vf;

	// No source_video at all (nullptr default) — same as every VOD/live call
	// site that hasn't probed one yet.
	pushVideoEncoderArgs(a, vf, HwAccel::nvidia, 2);
	EXPECT_EQ(std::find(a.begin(), a.end(), "-g"), a.end());

	a.clear();
	vf.clear();
	// A source_video that ffprobe genuinely had no basis to compute a rate
	// for — must not guess one rather than emit something wrong.
	VideoTrack unknown_fps;
	unknown_fps.r_frame_rate = "0/0";
	pushVideoEncoderArgs(a, vf, HwAccel::nvidia, 2, &unknown_fps);
	EXPECT_EQ(std::find(a.begin(), a.end(), "-g"), a.end());
}