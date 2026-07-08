#include <gtest/gtest.h>
#include "log/LogBuffer.h"
#include <vector>
#include <string>
#include <chrono>
#include <thread>

TEST(LogBufferTest, PushAndRecent) {
    LogBuffer buf;
    buf.push("line 1");
    buf.push("line 2");
    buf.push("line 3");

    auto [lines, seq] = buf.recent(2);
    ASSERT_EQ(lines.size(), 2);
    EXPECT_EQ(lines[0], "line 2");
    EXPECT_EQ(lines[1], "line 3");
    EXPECT_EQ(seq, 3);
}

TEST(LogBufferTest, WaitAfter) {
    LogBuffer buf;
    buf.push("line 1");

    // Start a thread to push a line after a short delay
    std::thread t([&buf]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        buf.push("line 2");
    });

    auto [lines, seq] = buf.waitAfter(1, std::chrono::milliseconds(500));
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0], "line 2");
    EXPECT_EQ(seq, 2);

    t.join();
}

TEST(LogBufferTest, WaitAfterTimeout) {
    LogBuffer buf;
    buf.push("line 1");

    auto [lines, seq] = buf.waitAfter(1, std::chrono::milliseconds(50));
    EXPECT_TRUE(lines.empty());
    EXPECT_EQ(seq, 1);
}

TEST(LogBufferTest, Overflow) {
    LogBuffer buf;
    // kMax is 2000
    for (int i = 0; i < 2100; ++i) {
        buf.push("line " + std::to_string(i));
    }

    auto [lines, seq] = buf.recent(5000);
    EXPECT_EQ(lines.size(), 2000);
    EXPECT_EQ(lines.front(), "line 100");
    EXPECT_EQ(lines.back(), "line 2099");
    EXPECT_EQ(seq, 2100); // seq_ starts at 1, so 2100 pushes end at seq 2100
}
