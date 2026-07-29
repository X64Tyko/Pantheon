#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Generic recurring-job scheduler — NOT to be confused with kairos/src/scheduler/,
// which is EPG/channel scheduling (RuleEngine, EPGMaterializer, TimeslotBlock).
// That's an unrelated meaning of "scheduler" this deliberately avoids sharing a
// directory with.
//
// A small, reusable "run this periodically" mechanism: replaces the one-off
// tick-counter that used to live inline in main.cpp's coordinator thread (see
// the playback-history prune migration, its first job) and is meant to be the
// landing spot for future periodic maintenance work (guest-account pruning,
// sync passes, backups) instead of each growing its own bespoke timer.
class JobScheduler
{
public:
	// now_fn returns milliseconds since epoch (same int64_t-ms convention as
	// the rest of Kairos, e.g. PlaybackHistoryRepository::prune) — lets tests
	// inject a controllable clock for registerDaily's wall-clock-time-of-day
	// math without sleeping a real day, and gives registerInterval real
	// sub-second precision instead of truncating to whole seconds.
	// Production code just uses the default (real time).
	explicit JobScheduler(std::function<int64_t()> now_fn = [] { return static_cast<int64_t>(std::time(nullptr)) * 1000; });
	~JobScheduler();

	JobScheduler(const JobScheduler&)            = delete;
	JobScheduler& operator=(const JobScheduler&) = delete;

	// Runs fn roughly every `interval`, starting one interval after
	// registration (not immediately — matches the existing playback-history
	// prune's own "don't do heavy work at boot" posture).
	void registerInterval(const std::string& name, std::chrono::milliseconds interval, std::function<void()> fn);

	// Runs fn once per day at hour:minute UTC. If registration happens after
	// that time today, the first run is tomorrow — never fires "late" for
	// today's already-passed slot on startup.
	void registerDaily(const std::string& name, int hour, int minute, std::function<void()> fn);

	// Starts the background thread (ticks once a second, running whichever
	// registered jobs are due). No-op if already started.
	void start();
	// Stops and joins the background thread. Safe to call even if start()
	// was never called (e.g. a test driving everything via tickOnceForTest
	// instead) or already stopped.
	void stop();

	// Test hook: runs whichever jobs are currently due, synchronously, on
	// the calling thread — lets tests assert on job execution deterministically
	// instead of racing the background thread's own sleep. Only meant for use
	// when start() was never called (calling it concurrently with a running
	// background thread races runDueJobs against itself).
	void tickOnceForTest();

private:
	struct Job
	{
		std::string name;
		std::function<void()> fn;
		int64_t next_due_ms;
		// Both interval and daily jobs re-derive their next due time as
		// next_due_ms + period_ms (drift-free — anchored to wall clock, not
		// accumulated scheduler-loop overhead). For a daily job, period_ms is
		// always exactly 86400000 (24h), which lands back on the same
		// hour:minute it started at.
		int64_t period_ms;
	};

	void runDueJobs();

	std::function<int64_t()> now_;
	std::mutex mtx_;
	std::vector<Job> jobs_;
	std::thread thread_;
	std::atomic<bool> stop_{true};
};