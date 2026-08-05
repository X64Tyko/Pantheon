#include <gtest/gtest.h>
#include "stream/ChannelViewerRegistry.h"
#include "stream/SessionManager.h"
#include "kairos/KairosClient.h"

// viewerCounts() coverage — added alongside the Activity page's exact
// per-channel/per-bucket viewer count (previously only client_count, exact
// but MPEG-TS/DVR-only, and hls_viewer_active, presence-only). Only exercises
// the default-bucket path (info=nullptr always resolves to kDefaultBucket,
// see ChannelViewerRegistry::start) — the native bucket path spins up a real
// ChannelSession via SessionManager::getOrCreate, which isn't worth doing in
// a unit test just to cover this aggregation method.
class ChannelViewerRegistryTest : public ::testing::Test
{
protected:
	KairosClient kairos{"http://unused.invalid"};
	SessionManager sessions{kairos, "ffmpeg", StreamOptions{}};
	ChannelViewerRegistry registry{sessions};
};

TEST_F(ChannelViewerRegistryTest, EmptyRegistryHasNoCounts)
{
	EXPECT_TRUE(registry.viewerCounts().empty());
}

TEST_F(ChannelViewerRegistryTest, CountsMultipleViewersOnSameChannelAndBucket)
{
	ClientCapabilities caps;
	registry.start("channel-a", caps, nullptr, 0);
	registry.start("channel-a", caps, nullptr, 0);
	registry.start("channel-a", caps, nullptr, 0);

	auto counts = registry.viewerCounts();
	ASSERT_EQ(counts["channel-a"][ChannelSession::kDefaultBucket], 3);
}

TEST_F(ChannelViewerRegistryTest, SeparatesCountsByChannel)
{
	ClientCapabilities caps;
	registry.start("channel-a", caps, nullptr, 0);
	registry.start("channel-b", caps, nullptr, 0);
	registry.start("channel-b", caps, nullptr, 0);

	auto counts = registry.viewerCounts();
	EXPECT_EQ(counts["channel-a"][ChannelSession::kDefaultBucket], 1);
	EXPECT_EQ(counts["channel-b"][ChannelSession::kDefaultBucket], 2);
}

TEST_F(ChannelViewerRegistryTest, StoppedViewerNoLongerCounted)
{
	ClientCapabilities caps;
	auto a = registry.start("channel-a", caps, nullptr, 0);
	registry.start("channel-a", caps, nullptr, 0);
	ASSERT_EQ(registry.viewerCounts()["channel-a"][ChannelSession::kDefaultBucket], 2);

	registry.stop(a.viewer_session_id);
	EXPECT_EQ(registry.viewerCounts()["channel-a"][ChannelSession::kDefaultBucket], 1);
}

// Regression coverage for the mid-stream bucket-swap bug: two independent
// ffmpeg encodes (default/native) got silently spliced under one already-
// polling viewer's manifest URL, showing up in production as one item's
// audio/video bleeding into the next at every switch. reassignForChannel()
// must never mutate ViewerEntry::bucket (what status()/touch() actually
// serve) — only recommended_bucket, which Hades polls and acts on via a real
// reconnect (a fresh start()).
namespace
{
	// A directly-streamable source: matches caps' declared codec/height, has
	// the requested audio track, and isn't VFR (r_frame_rate == avg_frame_rate).
	MediaInfo makeDirectStreamableInfo()
	{
		MediaInfo info;
		VideoTrack v;
		v.codec          = "h264";
		v.height         = 720;
		v.r_frame_rate   = "24000/1001";
		v.avg_frame_rate = "24000/1001";
		info.video.push_back(v);
		AudioTrack a;
		a.relative_index = 0;
		info.audio.push_back(a);
		return info;
	}

	ClientCapabilities makeMatchingCaps()
	{
		ClientCapabilities caps;
		caps.video_codecs = {"h264"};
		caps.max_height   = 1080;
		return caps;
	}

	constexpr int64_t kLongItemMs  = 60'000; // above kMinReassignDurationMs
	constexpr int64_t kShortItemMs = 10'000; // below it — a typical bumper
}

TEST_F(ChannelViewerRegistryTest, ReassignNeverMutatesServingBucket)
{
	ClientCapabilities caps = makeMatchingCaps();
	// No info at start() -> resolves to default, same as any unknown/offline connect.
	auto viewer = registry.start("channel-a", caps, nullptr, 0);
	ASSERT_EQ(viewer.bucket, ChannelSession::kDefaultBucket);

	// A long, directly-streamable item starts airing — ideal bucket is now
	// native for this viewer's caps, but the already-connected viewer's
	// pinned/serving bucket must not move.
	auto info = makeDirectStreamableInfo();
	registry.reassignForChannel("channel-a", info, 0, kLongItemMs, /*is_filler=*/false);

	auto st = registry.status(viewer.viewer_session_id);
	ASSERT_TRUE(st.has_value());
	EXPECT_EQ(st->bucket, ChannelSession::kDefaultBucket) << "serving bucket must stay pinned";
	EXPECT_EQ(st->recommended_bucket, ChannelSession::kNativeBucket) << "recommendation should reflect the new item";
}

TEST_F(ChannelViewerRegistryTest, ShortItemDoesNotChangeRecommendation)
{
	ClientCapabilities caps = makeMatchingCaps();
	auto viewer             = registry.start("channel-a", caps, nullptr, 0);

	auto info = makeDirectStreamableInfo();
	// A short bumper, even though directly-streamable, shouldn't flip the
	// recommendation — not worth a client-side reconnect for it.
	registry.reassignForChannel("channel-a", info, 0, kShortItemMs, /*is_filler=*/false);

	auto st = registry.status(viewer.viewer_session_id);
	ASSERT_TRUE(st.has_value());
	EXPECT_EQ(st->recommended_bucket, ChannelSession::kDefaultBucket)
		<< "a short item must not trigger a reconnect recommendation";
}

TEST_F(ChannelViewerRegistryTest, FillerItemDoesNotChangeRecommendationRegardlessOfDuration)
{
	ClientCapabilities caps = makeMatchingCaps();
	auto viewer             = registry.start("channel-a", caps, nullptr, 0);

	auto info = makeDirectStreamableInfo();
	// Even a long filler shouldn't recommend a switch — fillers are treated
	// the same as short items (see reassignForChannel's own comment).
	registry.reassignForChannel("channel-a", info, 0, kLongItemMs, /*is_filler=*/true);

	auto st = registry.status(viewer.viewer_session_id);
	ASSERT_TRUE(st.has_value());
	EXPECT_EQ(st->recommended_bucket, ChannelSession::kDefaultBucket);
}

// Regression coverage for the asymmetry between the two reassignment
// directions: unlike the opportunistic default->native upgrade (safe to
// defer for a short item — the viewer stays on the always-works transcode
// bucket in the meantime), a native->default fallback must never be
// debounced. A native session always does a raw stream copy of whatever's
// airing regardless of any viewer's real decode capabilities — the registry
// is the only thing that knows a given viewer can no longer safely watch it,
// so leaving that viewer pinned to native through a short-but-incompatible
// item would serve them genuinely undecodable video for its whole duration.
TEST_F(ChannelViewerRegistryTest, ShortIncompatibleItemStillRecommendsFallingBackFromNative)
{
	ClientCapabilities caps = makeMatchingCaps();
	auto streamableInfo     = makeDirectStreamableInfo();
	auto viewer             = registry.start("channel-a", caps, &streamableInfo, 0);
	ASSERT_EQ(viewer.bucket, ChannelSession::kNativeBucket);

	MediaInfo incompatible      = streamableInfo;
	incompatible.video[0].codec = "hevc"; // caps only declared h264
	registry.reassignForChannel("channel-a", incompatible, 0, kShortItemMs, /*is_filler=*/false);

	auto st = registry.status(viewer.viewer_session_id);
	ASSERT_TRUE(st.has_value());
	EXPECT_EQ(st->bucket, ChannelSession::kNativeBucket) << "serving bucket must stay pinned";
	EXPECT_EQ(st->recommended_bucket, ChannelSession::kDefaultBucket)
		<< "an incompatible item must recommend falling back regardless of duration";
}

TEST_F(ChannelViewerRegistryTest, ShortIncompatibleFillerStillRecommendsFallingBackFromNative)
{
	ClientCapabilities caps = makeMatchingCaps();
	auto streamableInfo     = makeDirectStreamableInfo();
	auto viewer             = registry.start("channel-a", caps, &streamableInfo, 0);
	ASSERT_EQ(viewer.bucket, ChannelSession::kNativeBucket);

	MediaInfo incompatible      = streamableInfo;
	incompatible.video[0].codec = "hevc";
	// Even a long, non-filler item wouldn't normally be debounced anyway —
	// this specifically checks a *filler* still recommends falling back,
	// since is_filler alone is not a reason to skip the safety fallback.
	registry.reassignForChannel("channel-a", incompatible, 0, kLongItemMs, /*is_filler=*/true);

	auto st = registry.status(viewer.viewer_session_id);
	ASSERT_TRUE(st.has_value());
	EXPECT_EQ(st->recommended_bucket, ChannelSession::kDefaultBucket);
}

TEST_F(ChannelViewerRegistryTest, ShortCompatibleItemDoesNotChangeNativePinnedViewersRecommendation)
{
	// Sanity check the other half of the asymmetry: a native-pinned viewer
	// whose short item is still compatible has nothing to fall back from, so
	// the recommendation should simply stay native.
	ClientCapabilities caps = makeMatchingCaps();
	auto streamableInfo     = makeDirectStreamableInfo();
	auto viewer             = registry.start("channel-a", caps, &streamableInfo, 0);
	ASSERT_EQ(viewer.bucket, ChannelSession::kNativeBucket);

	registry.reassignForChannel("channel-a", streamableInfo, 0, kShortItemMs, false);

	auto st = registry.status(viewer.viewer_session_id);
	ASSERT_TRUE(st.has_value());
	EXPECT_EQ(st->recommended_bucket, ChannelSession::kNativeBucket);
}

TEST_F(ChannelViewerRegistryTest, RecommendationReturnsToDefaultWhenItemNoLongerStreamable)
{
	ClientCapabilities caps = makeMatchingCaps();
	auto viewer             = registry.start("channel-a", caps, nullptr, 0);

	auto streamable = makeDirectStreamableInfo();
	registry.reassignForChannel("channel-a", streamable, 0, kLongItemMs, false);
	ASSERT_EQ(registry.status(viewer.viewer_session_id)->recommended_bucket, ChannelSession::kNativeBucket);

	// Next item doesn't match this viewer's declared caps (wrong codec) —
	// recommendation should fall back to default, still without moving the
	// pinned serving bucket.
	MediaInfo notStreamable      = streamable;
	notStreamable.video[0].codec = "hevc"; // caps only declared h264
	registry.reassignForChannel("channel-a", notStreamable, 0, kLongItemMs, false);

	auto st = registry.status(viewer.viewer_session_id);
	EXPECT_EQ(st->bucket, ChannelSession::kDefaultBucket);
	EXPECT_EQ(st->recommended_bucket, ChannelSession::kDefaultBucket);
}

TEST_F(ChannelViewerRegistryTest, StatusUnknownViewerReturnsNullopt)
{
	EXPECT_FALSE(registry.status("nonexistent-viewer-id").has_value());
}