#include <gtest/gtest.h>
#include "stream/HwProbe.h"
#include <vector>
#include <string>
#include <algorithm>

TEST(HwProbeTest, MentionsHwaccelFailure) {
    EXPECT_TRUE(mentionsHwaccelFailureForTest("hwaccel initialisation returned error"));
    EXPECT_TRUE(mentionsHwaccelFailureForTest("Failed setup for format cuda"));
    EXPECT_TRUE(mentionsHwaccelFailureForTest("No usable encoding entrypoint found"));
    EXPECT_TRUE(mentionsHwaccelFailureForTest("Function not implemented"));
    
    EXPECT_FALSE(mentionsHwaccelFailureForTest("everything is fine"));
    EXPECT_FALSE(mentionsHwaccelFailureForTest(""));
}

TEST(HwProbeTest, BuildEncodeProbeArgs) {
    auto args = buildEncodeProbeArgsForTest(HwAccel::nvidia, "ffmpeg", "/dev/dri/renderD128");
    
    // Should have ffmpeg, -v error
    EXPECT_EQ(args[0], "ffmpeg");
    EXPECT_EQ(args[1], "-v");
    EXPECT_EQ(args[2], "error");
    
    // Should have nvenc encoder
    auto it = std::find(args.begin(), args.end(), "h264_nvenc");
    EXPECT_NE(it, args.end());
    
    // Should have nullsrc input
    auto it2 = std::find(args.begin(), args.end(), "-i");
    ASSERT_NE(it2, args.end());
    EXPECT_TRUE((it2 + 1)->find("nullsrc") != std::string::npos);
}

TEST(HwProbeTest, BuildDecodeProbeArgs) {
    auto args = buildDecodeProbeArgsForTest(HwAccel::amd, "ffmpeg", "/dev/dri/renderD128", "sample.mp4");
    
    // Should have -vaapi_device because it's amd
    auto it = std::find(args.begin(), args.end(), "-vaapi_device");
    EXPECT_NE(it, args.end());
    
    // Should have -hwaccel vaapi
    auto it2 = std::find(args.begin(), args.end(), "-hwaccel");
    ASSERT_NE(it2, args.end());
    EXPECT_EQ(*(it2 + 1), "vaapi");
    
    // Should have sample.mp4 input
    auto it3 = std::find(args.begin(), args.end(), "-i");
    ASSERT_NE(it3, args.end());
    EXPECT_EQ(*(it3 + 1), "sample.mp4");
}
