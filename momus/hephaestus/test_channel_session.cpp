#include <gtest/gtest.h>
#include "stream/ChannelSession.h"
#include "kairos/KairosTypes.h"

TEST(ChannelSessionTest, ComputeOffset) {
    KairosNowResponse item;
    item.wall_clock_start_ms = 1000;
    item.duration_ms = 5000;
    item.is_filler = false;

    // Normal item: at start
    EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 1000), 0);
    // Normal item: 2s in
    EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 3000), 2000);
    // Normal item: behind start (should clamp to 0 per impl)
    EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 500), 0);
}

TEST(ChannelSessionTest, ComputeOffsetFiller) {
    KairosNowResponse item;
    item.wall_clock_start_ms = 1000;
    item.duration_ms = 5000;
    item.is_filler = true;

    // Filler: first loop
    EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 3000), 2000);
    // Filler: second loop
    EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 7000), 1000);
    // Filler: third loop
    EXPECT_EQ(ChannelSession::computeOffsetForTest(item, 11000), 0);
}

TEST(ChannelSessionTest, ComputeSpeed) {
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
