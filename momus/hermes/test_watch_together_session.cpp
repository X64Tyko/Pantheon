#include <gtest/gtest.h>
#include "../../hermes/src/watchtogether/WatchTogetherSession.h"
#include <chrono>
#include <future>
#include <thread>

// ---- construction / initialSnapshot -------------------------------------

TEST(WatchTogetherSessionTest, FreshSessionSnapshotIsZeroAndPaused) {
    WatchTogetherSession session("wt-1", "host-a");

    EXPECT_EQ(session.sessionId(), "wt-1");
    EXPECT_EQ(session.hostUserId(), "host-a");

    auto [snapshot, seq] = session.initialSnapshot();
    EXPECT_EQ(snapshot["type"], "sync");
    EXPECT_EQ(snapshot["position_ms"], 0);
    EXPECT_EQ(snapshot["paused"], true);
    // Nothing appended yet — cursor starts at "nothing seen" (seq_ - 1 == 0).
    EXPECT_EQ(seq, 0u);
}

// A session that never gets a join (host-only, solo viewer) must still
// snapshot the live position correctly — there's nothing join-specific about
// initialSnapshot()'s correctness, it just reflects whatever pushCommand/
// heartbeat has set so far.
TEST(WatchTogetherSessionTest, SoloHostSessionSnapshotsLiveStateWithoutAnyJoin) {
    WatchTogetherSession session("wt-solo", "host-a");
    session.pushCommand({{"type", "play"}, {"position_ms", 5000}});

    auto [snapshot, seq] = session.initialSnapshot();
    EXPECT_EQ(snapshot["paused"], false);
    EXPECT_GE(snapshot["position_ms"].get<int64_t>(), 5000);
    EXPECT_EQ(seq, 1u); // one command already appended
}

// ---- pushCommand / authoritative state -----------------------------------

TEST(WatchTogetherSessionTest, PushCommandSeekUpdatesPositionButNotPaused) {
    WatchTogetherSession session("wt-1", "host-a");
    session.pushCommand({{"type", "play"}, {"position_ms", 1000}});
    ASSERT_FALSE(session.isPaused());

    session.pushCommand({{"type", "seek"}, {"position_ms", 42000}});
    EXPECT_FALSE(session.isPaused()); // seek must not implicitly pause/unpause
    EXPECT_GE(session.authoritativePositionMs(), 42000);
}

TEST(WatchTogetherSessionTest, PushCommandPlayAndPauseTogglePausedState) {
    WatchTogetherSession session("wt-1", "host-a");
    EXPECT_TRUE(session.isPaused()); // default

    session.pushCommand({{"type", "play"}, {"position_ms", 0}});
    EXPECT_FALSE(session.isPaused());

    session.pushCommand({{"type", "pause"}, {"position_ms", 3000}});
    EXPECT_TRUE(session.isPaused());
    EXPECT_EQ(session.authoritativePositionMs(), 3000); // flat while paused
}

TEST(WatchTogetherSessionTest, AuthoritativePositionExtrapolatesOnlyWhilePlaying) {
    WatchTogetherSession session("wt-1", "host-a");
    session.pushCommand({{"type", "pause"}, {"position_ms", 10000}});
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // Paused — position must stay flat regardless of elapsed wall time.
    EXPECT_EQ(session.authoritativePositionMs(), 10000);

    session.pushCommand({{"type", "play"}, {"position_ms", 10000}});
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // Playing — position must have advanced roughly with wall-clock time.
    EXPECT_GT(session.authoritativePositionMs(), 10000);
}

// ---- heartbeat --------------------------------------------------------

TEST(WatchTogetherSessionTest, HeartbeatUpdatesStateButAppendsNoEvent) {
    WatchTogetherSession session("wt-1", "host-a");
    session.heartbeat(7000, false);

    EXPECT_FALSE(session.isPaused());
    EXPECT_GE(session.authoritativePositionMs(), 7000);

    // No event should have landed on the log — heartbeat is baseline-only;
    // WatchTogetherManager's own tick is what turns it into a broadcast.
    auto [events, seq] = session.waitAfter(0, std::chrono::milliseconds(20));
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(seq, 0u);
}

TEST(WatchTogetherSessionTest, HeartbeatRefreshesHostLiveness) {
    WatchTogetherSession session("wt-1", "host-a");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_GE(session.hostIdleMs(), 50);

    session.heartbeat(1000, true);
    EXPECT_LT(session.hostIdleMs(), 50);
}

TEST(WatchTogetherSessionTest, PushCommandAlsoRefreshesHostLiveness) {
    WatchTogetherSession session("wt-1", "host-a");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_GE(session.hostIdleMs(), 50);

    session.pushCommand({{"type", "play"}});
    EXPECT_LT(session.hostIdleMs(), 50);
}

// ---- appendSyncEvent ----------------------------------------------------

TEST(WatchTogetherSessionTest, AppendSyncEventCarriesCurrentAuthoritativeState) {
    WatchTogetherSession session("wt-1", "host-a");
    session.pushCommand({{"type", "play"}, {"position_ms", 2000}});
    auto before_seq = session.initialSnapshot().second;

    session.appendSyncEvent();

    auto [events, new_seq] = session.waitAfter(before_seq, std::chrono::milliseconds(20));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0]["type"], "sync");
    EXPECT_FALSE(events[0]["paused"].get<bool>());
    EXPECT_GT(new_seq, before_seq);
}

// ---- waitAfter / event ordering ------------------------------------------

TEST(WatchTogetherSessionTest, WaitAfterReturnsOnlyEventsNewerThanCursor) {
    WatchTogetherSession session("wt-1", "host-a");
    session.pushCommand({{"type", "play"}, {"position_ms", 0}});   // seq 1
    session.pushCommand({{"type", "seek"}, {"position_ms", 5000}}); // seq 2
    session.pushCommand({{"type", "pause"}, {"position_ms", 6000}}); // seq 3

    auto [events, new_seq] = session.waitAfter(1, std::chrono::milliseconds(50));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0]["type"], "seek");
    EXPECT_EQ(events[1]["type"], "pause");
    EXPECT_EQ(new_seq, 3u);
}

TEST(WatchTogetherSessionTest, ReCallingWaitAfterWithNewCursorSkipsAlreadySeenEvents) {
    WatchTogetherSession session("wt-1", "host-a");
    session.pushCommand({{"type", "play"}, {"position_ms", 0}});    // seq 1
    session.pushCommand({{"type", "seek"}, {"position_ms", 1000}}); // seq 2

    auto [first_events, first_seq] = session.waitAfter(0, std::chrono::milliseconds(50));
    ASSERT_EQ(first_events.size(), 2u);
    EXPECT_EQ(first_seq, 2u);

    // A third event lands after the first read.
    session.pushCommand({{"type", "pause"}, {"position_ms", 1500}}); // seq 3

    auto [second_events, second_seq] = session.waitAfter(first_seq, std::chrono::milliseconds(50));
    ASSERT_EQ(second_events.size(), 1u);
    EXPECT_EQ(second_events[0]["type"], "pause");
    EXPECT_EQ(second_seq, 3u);
}

TEST(WatchTogetherSessionTest, WaitAfterTimesOutEmptyWithSameSeqWhenNoNewEvents) {
    WatchTogetherSession session("wt-1", "host-a");
    session.pushCommand({{"type", "play"}, {"position_ms", 0}}); // seq 1

    auto start = std::chrono::steady_clock::now();
    auto [events, seq] = session.waitAfter(1, std::chrono::milliseconds(100));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_TRUE(events.empty());
    EXPECT_EQ(seq, 1u); // unchanged — no new events to advance the cursor
    EXPECT_GE(elapsed, 85); // allow small scheduling slack under 100ms
}

// Mirrors DeviceSessionManagerTest.PushCommandWakesWaitingLongPoll — a
// blocked waitAfter() must wake as soon as a new event lands, not only after
// the full timeout elapses.
TEST(WatchTogetherSessionTest, WaitAfterWakesImmediatelyOnNewEvent) {
    WatchTogetherSession session("wt-1", "host-a");

    auto fut = std::async(std::launch::async, [&] {
        return session.waitAfter(0, std::chrono::milliseconds(5000));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    session.pushCommand({{"type", "play"}, {"position_ms", 0}});

    auto [events, seq] = fut.get();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0]["type"], "play");
    EXPECT_EQ(seq, 1u);
}

// Two commands issued back-to-back with no gap must still land as two
// distinct, strictly-ordered, monotonically increasing sequence numbers —
// the later one is never silently dropped or coalesced.
TEST(WatchTogetherSessionTest, RapidSeekThenPauseAreBothPreservedInOrder) {
    WatchTogetherSession session("wt-1", "host-a");
    session.pushCommand({{"type", "seek"}, {"position_ms", 8000}});
    session.pushCommand({{"type", "pause"}, {"position_ms", 8000}});

    auto [events, seq] = session.waitAfter(0, std::chrono::milliseconds(50));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0]["type"], "seek");
    EXPECT_EQ(events[1]["type"], "pause");
    EXPECT_EQ(seq, 2u);
    // The later event (pause) is what determines final authoritative state.
    EXPECT_TRUE(session.isPaused());
}

// The event log caps backlog at a small bound (kMaxBacklog=20 in the header)
// — a slow/never-polling follower must not make the log grow unbounded.
// Sequence numbers must stay correct even after older entries are evicted.
TEST(WatchTogetherSessionTest, EventLogBacklogIsBoundedButSeqStaysMonotonic) {
    WatchTogetherSession session("wt-1", "host-a");
    for (int i = 0; i < 30; ++i) {
        session.pushCommand({{"type", "seek"}, {"position_ms", i * 1000}});
    }

    auto [events, seq] = session.waitAfter(0, std::chrono::milliseconds(50));
    // Backlog trimmed to at most kMaxBacklog entries, not all 30.
    EXPECT_LE(events.size(), 20u);
    EXPECT_EQ(seq, 30u);

    // Asking from a cursor far in the past (already evicted) still returns
    // whatever survives in the backlog rather than erroring.
    auto [events2, seq2] = session.waitAfter(5, std::chrono::milliseconds(50));
    EXPECT_EQ(seq2, 30u);
    EXPECT_FALSE(events2.empty());
}

// ---- hostAuthHeader -------------------------------------------------------

TEST(WatchTogetherSessionTest, HostAuthHeaderEmptyUntilSet) {
    WatchTogetherSession session("wt-1", "host-a");
    EXPECT_TRUE(session.hostAuthHeader().empty());

    session.setHostAuthHeader("Bearer abc123");
    EXPECT_EQ(session.hostAuthHeader(), "Bearer abc123");
}
