#include <gtest/gtest.h>
#include "../../hermes/src/devices/DeviceSessionManager.h"
#include <chrono>
#include <future>
#include <thread>

TEST(DeviceSessionManagerTest, GetOrCreateAndReuse) {
    DeviceSessionManager mgr;

    auto s1 = mgr.getOrCreate("dev-1", "user-a");
    auto s2 = mgr.getOrCreate("dev-1", "user-a");
    auto s3 = mgr.getOrCreate("dev-2", "user-a");

    EXPECT_EQ(s1, s2);
    EXPECT_NE(s1, s3);
    EXPECT_EQ(s1->rokuDeviceId(), "dev-1");
    EXPECT_EQ(s1->userId(), "user-a");
}

TEST(DeviceSessionManagerTest, GetReturnsNullForUnknownDevice) {
    DeviceSessionManager mgr;
    EXPECT_EQ(mgr.get("nope"), nullptr);
}

TEST(DeviceSessionManagerTest, ListForUserOnlyReturnsOwnedSessions) {
    DeviceSessionManager mgr;
    mgr.getOrCreate("dev-1", "user-a");
    mgr.getOrCreate("dev-2", "user-a");
    mgr.getOrCreate("dev-3", "user-b");

    auto forA = mgr.listForUser("user-a");
    auto forB = mgr.listForUser("user-b");
    EXPECT_EQ(forA.size(), 2u);
    EXPECT_EQ(forB.size(), 1u);
    EXPECT_EQ(forB[0]->rokuDeviceId(), "dev-3");
}

// The long-poll round trip: pushCommand from the "caster" side should wake
// waitForCommand on the "Roku" side immediately, not after the timeout.
TEST(DeviceSessionManagerTest, PushCommandWakesWaitingLongPoll) {
    DeviceSessionManager mgr;
    auto session = mgr.getOrCreate("dev-1", "user-a");

    auto fut = std::async(std::launch::async, [&] {
        return session->waitForCommand(5000);
    });

    // Give the waiter a moment to actually start blocking before pushing.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    session->pushCommand({{"type", "play"}});

    auto result = fut.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)["type"], "play");
}

TEST(DeviceSessionManagerTest, WaitForCommandTimesOutWithNoneQueued) {
    DeviceSessionManager mgr;
    auto session = mgr.getOrCreate("dev-1", "user-a");

    auto start = std::chrono::steady_clock::now();
    auto result = session->waitForCommand(100);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    EXPECT_FALSE(result.has_value());
    EXPECT_GE(elapsed, 90); // allow a little scheduling slack under 100ms
}

TEST(DeviceSessionManagerTest, StateRoundTrips) {
    DeviceSessionManager mgr;
    auto session = mgr.getOrCreate("dev-1", "user-a");

    EXPECT_TRUE(session->state().empty());
    session->setState({{"playing", true}, {"positionMs", 1500}});
    EXPECT_EQ(session->state()["playing"], true);
    EXPECT_EQ(session->state()["positionMs"], 1500);
}

TEST(DeviceSessionManagerTest, ReapDropsOnlyStaleSessions) {
    DeviceSessionManager mgr;
    auto stale = mgr.getOrCreate("dev-stale", "user-a");
    (void)stale;

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Freshly touched right before reaping — must survive a 100ms grace
    // period even though dev-stale (untouched since creation) does not.
    auto fresh = mgr.getOrCreate("dev-fresh", "user-a");
    (void)fresh;

    mgr.reap(100);

    EXPECT_EQ(mgr.get("dev-stale"), nullptr);
    EXPECT_NE(mgr.get("dev-fresh"), nullptr);
}
