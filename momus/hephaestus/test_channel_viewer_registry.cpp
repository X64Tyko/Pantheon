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