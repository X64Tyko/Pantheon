#include <gtest/gtest.h>
#include "normalize/MediaNormalizer.h"

// needsNormalize() is pure — no ffmpeg/ffprobe — exercised directly against
// synthetic CodecSummary values.

TEST(NeedsNormalize, NoVideoStreamIsSkipped)
{
	CodecSummary s;
	s.has_video = false;
	EXPECT_FALSE(needsNormalize(s));
}

TEST(NeedsNormalize, AlreadyH264AacCfrNeedsNothing)
{
	CodecSummary s;
	s.has_video   = true;
	s.video_codec = "h264";
	s.audio_codec = "aac";
	s.likely_vfr  = false;
	EXPECT_FALSE(needsNormalize(s));
}

TEST(NeedsNormalize, NonH264VideoNeedsNormalize)
{
	CodecSummary s;
	s.has_video   = true;
	s.video_codec = "mpeg4";
	s.audio_codec = "aac";
	EXPECT_TRUE(needsNormalize(s));
}

TEST(NeedsNormalize, NonAacAudioNeedsNormalize)
{
	CodecSummary s;
	s.has_video   = true;
	s.video_codec = "h264";
	s.audio_codec = "mp3";
	EXPECT_TRUE(needsNormalize(s));
}

TEST(NeedsNormalize, LikelyVfrNeedsNormalizeEvenWithConformingCodecs)
{
	CodecSummary s;
	s.has_video   = true;
	s.video_codec = "h264";
	s.audio_codec = "aac";
	s.likely_vfr  = true;
	EXPECT_TRUE(needsNormalize(s));
}