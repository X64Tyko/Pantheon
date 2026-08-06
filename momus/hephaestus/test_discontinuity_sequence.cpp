#include <gtest/gtest.h>
#include "stream/ChannelSession.h"

// patchDiscontinuitySequenceForTest() is now a pure function (no file I/O) —
// see ChannelSession.h's comment on why this moved off a background loop
// that used to rewrite playlist.m3u8 on disk (a second, uncoordinated writer
// racing ffmpeg's own HLS muxer). These tests exercise it directly on
// in-memory playlist text instead of writing fixture files.

TEST(DiscontinuitySequenceTest, InsertsSequenceWhenDiscontinuityHasRolledOff)
{
	// Exactly the reported bug: a discontinuity happened once (count=1) but
	// its #EXT-X-DISCONTINUITY tag has already scrolled out of the current
	// 6-segment window — none of these segments carry it anymore.
	std::string raw =
		"#EXTM3U\n"
		"#EXT-X-VERSION:3\n"
		"#EXT-X-TARGETDURATION:2\n"
		"#EXT-X-MEDIA-SEQUENCE:10\n"
		"#EXTINF:2.000,\n"
		"seg-00010.ts\n"
		"#EXTINF:2.000,\n"
		"seg-00011.ts\n";

	std::string result = ChannelSession::patchDiscontinuitySequenceForTest(raw, 1);

	EXPECT_NE(result.find("#EXT-X-DISCONTINUITY-SEQUENCE:1"), std::string::npos);
	// Placement: after #EXT-X-VERSION, still before the first segment.
	auto versionPos  = result.find("#EXT-X-VERSION");
	auto seqPos      = result.find("#EXT-X-DISCONTINUITY-SEQUENCE");
	auto firstSegPos = result.find("#EXTINF");
	EXPECT_LT(versionPos, seqPos);
	EXPECT_LT(seqPos, firstSegPos);
}

TEST(DiscontinuitySequenceTest, ZeroWhenDiscontinuityStillVisible)
{
	// Right after a transition — the tag is still inside the window, so the
	// *cumulative rolled-off* count is correctly 0 even though one has
	// happened in this session's lifetime.
	std::string raw =
		"#EXTM3U\n"
		"#EXT-X-VERSION:3\n"
		"#EXT-X-TARGETDURATION:2\n"
		"#EXT-X-MEDIA-SEQUENCE:10\n"
		"#EXTINF:2.000,\n"
		"seg-00010.ts\n"
		"#EXT-X-DISCONTINUITY\n"
		"#EXTINF:2.000,\n"
		"seg-00011.ts\n";

	std::string result = ChannelSession::patchDiscontinuitySequenceForTest(raw, 1);

	EXPECT_NE(result.find("#EXT-X-DISCONTINUITY-SEQUENCE:0"), std::string::npos);
	// The still-visible tag itself must survive the rewrite untouched.
	EXPECT_NE(result.find("#EXT-X-DISCONTINUITY\n"), std::string::npos);
}

TEST(DiscontinuitySequenceTest, CorrectsStaleExistingValue)
{
	// A second discontinuity has since rolled off too — an existing (now
	// stale) sequence line must be updated, not left alone or duplicated.
	std::string raw =
		"#EXTM3U\n"
		"#EXT-X-VERSION:3\n"
		"#EXT-X-DISCONTINUITY-SEQUENCE:1\n"
		"#EXT-X-TARGETDURATION:2\n"
		"#EXTINF:2.000,\n"
		"seg-00020.ts\n";

	std::string result = ChannelSession::patchDiscontinuitySequenceForTest(raw, 2);

	EXPECT_EQ(result.find("#EXT-X-DISCONTINUITY-SEQUENCE:1"), std::string::npos);
	EXPECT_NE(result.find("#EXT-X-DISCONTINUITY-SEQUENCE:2"), std::string::npos);
	// Never two sequence lines.
	size_t first = result.find("#EXT-X-DISCONTINUITY-SEQUENCE:");
	EXPECT_EQ(result.find("#EXT-X-DISCONTINUITY-SEQUENCE:", first + 1), std::string::npos);
}

TEST(DiscontinuitySequenceTest, IsIdempotentWhenAlreadyCorrect)
{
	std::string raw =
		"#EXTM3U\n"
		"#EXT-X-VERSION:3\n"
		"#EXT-X-DISCONTINUITY-SEQUENCE:1\n"
		"#EXTINF:2.000,\n"
		"seg-00020.ts\n";

	std::string once  = ChannelSession::patchDiscontinuitySequenceForTest(raw, 1);
	std::string twice = ChannelSession::patchDiscontinuitySequenceForTest(once, 1);

	EXPECT_EQ(once, twice);
}

TEST(DiscontinuitySequenceTest, LeavesGarbledInputUntouched)
{
	// No #EXTM3U header — simulates catching a partial/garbled read. Must
	// return the input unchanged rather than "fixing" it into something new.
	std::string raw = "#EXTINF:2.000,\nseg-00099.ts\n";

	std::string result = ChannelSession::patchDiscontinuitySequenceForTest(raw, 3);

	EXPECT_EQ(result, raw);
}