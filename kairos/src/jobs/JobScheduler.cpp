#include "JobScheduler.h"
#include <algorithm>
#include <iostream>

namespace
{
	// Next UTC epoch-milliseconds instant at hour:minute — today if that time
	// hasn't passed yet, tomorrow otherwise. Never returns an instant <= now,
	// so a job registered right after its daily slot already passed today
	// waits for tomorrow instead of firing immediately.
	int64_t nextDailyOccurrence(int64_t now_ms, int hour, int minute)
	{
		std::time_t t = static_cast<std::time_t>(now_ms / 1000);
		std::tm tm_utc{};
		gmtime_r(&t, &tm_utc);
		tm_utc.tm_hour       = hour;
		tm_utc.tm_min        = minute;
		tm_utc.tm_sec        = 0;
		int64_t candidate_ms = static_cast<int64_t>(timegm(&tm_utc)) * 1000;
		if (candidate_ms <= now_ms) candidate_ms += 24LL * 3600 * 1000;
		return candidate_ms;
	}
} // namespace

JobScheduler::JobScheduler(std::function<int64_t()> now_fn)
	: now_(std::move(now_fn))
{
}

JobScheduler::~JobScheduler()
{
	stop();
}

void JobScheduler::registerInterval(const std::string& name, std::chrono::milliseconds interval, std::function<void()> fn)
{
	const int64_t period_ms = std::max<int64_t>(1, interval.count());
	std::lock_guard<std::mutex> lock(mtx_);
	jobs_.push_back(Job{name, std::move(fn), now_() + period_ms, period_ms});
}

void JobScheduler::registerDaily(const std::string& name, int hour, int minute, std::function<void()> fn)
{
	std::lock_guard<std::mutex> lock(mtx_);
	const int64_t first_due_ms = nextDailyOccurrence(now_(), hour, minute);
	jobs_.push_back(Job{name, std::move(fn), first_due_ms, 24LL * 3600 * 1000});
}

void JobScheduler::runDueJobs()
{
	const int64_t now = now_();
	std::lock_guard<std::mutex> lock(mtx_);
	for (auto& job : jobs_)
	{
		if (now < job.next_due_ms) continue;
		try
		{
			job.fn();
		}
		catch (const std::exception& e)
		{
			std::cerr << "[kairos] job '" << job.name << "' threw: " << e.what() << std::endl;
		}
		catch (...)
		{
			std::cerr << "[kairos] job '" << job.name << "' threw an unknown exception" << std::endl;
		}
		// Re-anchor on the actual now rather than the missed due time, so a
		// job that was briefly delayed (process paused, a slow prior job)
		// doesn't then fire in a tight catch-up burst.
		job.next_due_ms = now + job.period_ms;
	}
}

void JobScheduler::tickOnceForTest()
{
	runDueJobs();
}

void JobScheduler::start()
{
	stop_.store(false);
	thread_ = std::thread([this]
	{
		// 50ms poll granularity: negligible CPU cost for a background
		// maintenance thread, and fine enough that production jobs (minutes/
		// hours/day scale) never notice the latency while still letting
		// tests exercise fast intervals without waiting real minutes.
		using namespace std::chrono_literals;
		while (!stop_.load(std::memory_order_relaxed))
		{
			runDueJobs();
			std::this_thread::sleep_for(50ms);
		}
	});
}

void JobScheduler::stop()
{
	stop_.store(true);
	if (thread_.joinable()) thread_.join();
}