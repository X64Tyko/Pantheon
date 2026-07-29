#include <gtest/gtest.h>
#include "jobs/JobScheduler.h"

#include <atomic>
#include <chrono>
#include <thread>

// registerInterval firing is tested against the real background thread with
// short intervals + generous margins (same style as
// momus/hermes/test_watch_together_session.cpp's real-time-based tests) —
// interval firing is about relative elapsed time, not a specific wall-clock
// instant, so a real short sleep is a faithful, non-flaky test of it.
//
// registerDaily is tested via tickOnceForTest() with an injected clock
// instead, since its whole point is wall-clock-time-of-day math that a real
// sleep can't exercise without waiting a real day.

// A real-thread smoke test: proves start() actually drives registered work
// off the background thread at all. Deliberately not the test proving
// *repeated* firing — real-clock sleeps are too susceptible to scheduling
// contention on a loaded box to reliably observe several firings in a short
// window (see IntervalJobFiresMultipleTimesAsSimulatedClockAdvances below for
// the deterministic version of that, driven by tickOnceForTest() instead).
TEST(JobSchedulerTest, IntervalJobFiresOnRealBackgroundThread)
{
	JobScheduler sched;
	std::atomic<int> calls{0};
	sched.registerInterval("test", std::chrono::milliseconds(5), [&] { calls.fetch_add(1); });
	sched.start();
	std::this_thread::sleep_for(std::chrono::seconds(2));
	sched.stop();
	EXPECT_GE(calls.load(), 1);
}

TEST(JobSchedulerTest, IntervalJobFiresMultipleTimesAsSimulatedClockAdvances)
{
	int64_t now = 0;
	JobScheduler sched([&] { return now; });
	int calls = 0;
	sched.registerInterval("test", std::chrono::milliseconds(10), [&] { ++calls; });

	for (int i = 1; i <= 5; ++i)
	{
		now += 10;
		sched.tickOnceForTest();
		EXPECT_EQ(calls, i);
	}
}

TEST(JobSchedulerTest, IntervalJobDoesNotFireBeforeItsFirstInterval)
{
	JobScheduler sched;
	std::atomic<int> calls{0};
	sched.registerInterval("test", std::chrono::seconds(60), [&] { calls.fetch_add(1); });
	sched.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	sched.stop();
	EXPECT_EQ(calls.load(), 0);
}

TEST(JobSchedulerTest, DailyJobDoesNotFireBeforeItsScheduledTime)
{
	// A fixed "now" — an arbitrary anchor instant, in epoch-milliseconds
	// (JobScheduler's now_fn convention).
	int64_t now = 1'700'000'400'000;
	JobScheduler sched([&] { return now; });
	int calls = 0;
	// Schedule for 1 minute from "now" — still in the future, should not fire yet.
	std::time_t t = static_cast<std::time_t>(now / 1000);
	std::tm tm_utc{};
	gmtime_r(&t, &tm_utc);
	int hour = tm_utc.tm_hour, minute = (tm_utc.tm_min + 1) % 60;
	sched.registerDaily("test", hour, minute, [&] { ++calls; });
	sched.tickOnceForTest();
	EXPECT_EQ(calls, 0);
}

TEST(JobSchedulerTest, DailyJobFiresOnceItsScheduledTimeArrivesAndReschedulesADayLater)
{
	int64_t now = 1'700'000'000'000;
	JobScheduler sched([&] { return now; });
	int calls = 0;

	std::time_t t = static_cast<std::time_t>(now / 1000);
	std::tm tm_utc{};
	gmtime_r(&t, &tm_utc);
	// Schedule for right now (this minute) — registerDaily computes "next
	// occurrence" as <= now meaning "already passed", so scheduling exactly
	// at "now" isn't representable as still-pending; instead schedule for
	// the current hour:minute, then advance the clock by exactly one day and
	// confirm it fires at that point, then again a day after that.
	sched.registerDaily("test", tm_utc.tm_hour, tm_utc.tm_min, [&] { ++calls; });

	now += 24LL * 3600 * 1000; // exactly one day later — the job's first due instant
	sched.tickOnceForTest();
	EXPECT_EQ(calls, 1);

	// A moment later, still the same day — should not fire again.
	now += 5000;
	sched.tickOnceForTest();
	EXPECT_EQ(calls, 1);

	// Another full day later — fires again.
	now += 24LL * 3600 * 1000;
	sched.tickOnceForTest();
	EXPECT_EQ(calls, 2);
}

TEST(JobSchedulerTest, OneJobThrowingDoesNotPreventOtherJobsFromRunning)
{
	int64_t now = 1'000'000;
	JobScheduler sched([&] { return now; });
	int good_calls = 0;

	sched.registerInterval("throws", std::chrono::seconds(1), [] { throw std::runtime_error("boom"); });
	sched.registerInterval("good", std::chrono::seconds(1), [&] { ++good_calls; });

	now += 2000;
	EXPECT_NO_THROW(sched.tickOnceForTest());
	EXPECT_EQ(good_calls, 1);

	now += 2000;
	EXPECT_NO_THROW(sched.tickOnceForTest());
	EXPECT_EQ(good_calls, 2);
}

TEST(JobSchedulerTest, MultipleRegisteredJobsAllRunIndependently)
{
	int64_t now = 0;
	JobScheduler sched([&] { return now; });
	int a = 0, b = 0;
	sched.registerInterval("a", std::chrono::seconds(10), [&] { ++a; });
	sched.registerInterval("b", std::chrono::seconds(30), [&] { ++b; });

	now += 10'000;
	sched.tickOnceForTest();
	EXPECT_EQ(a, 1);
	EXPECT_EQ(b, 0); // not due yet

	now += 20'000;
	sched.tickOnceForTest();
	EXPECT_EQ(a, 2); // due again at t=20s
	EXPECT_EQ(b, 1); // now due at t=30s
}