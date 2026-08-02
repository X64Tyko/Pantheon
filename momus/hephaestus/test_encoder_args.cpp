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

	// true -> -v verbose
	pushLogLevelArgs(a, true);
	ASSERT_EQ(a.size(), 2);
	EXPECT_EQ(a[0], "-v");
	EXPECT_EQ(a[1], "verbose");
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