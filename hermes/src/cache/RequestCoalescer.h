#pragma once
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Collapses concurrent Hermes-side upstream fetches for the same key into a
// single actual fetch — the "singleflight" pattern. Exists because
// proxyRequest() (Router.cpp) previously issued one independent
// httplib::Client fetch to Hephaestus per client request with zero sharing,
// so N simultaneous viewers polling for the same not-yet-cached live-channel
// segment each triggered N redundant upstream fetches — the actual
// duplicated cost for a popular demo channel with many concurrent viewers.
//
// Distinct from SegmentCache (shared/cache/): this only dedupes *concurrent
// in-flight* requests. Once a fetch completes and lands in the cache, later
// requests are cache hits and never reach this class at all.
struct CoalescedResult
{
	int status = 0;
	std::string body;
	std::string content_type;
	std::string cache_control;
};

class RequestCoalescer
{
public:
	struct Ticket
	{
		// true: caller is the leader for this key — it must actually perform
		// the fetch and call complete(key, ...) exactly once afterwards
		// (success or failure) so any followers unblock. false: another
		// request for the same key is already in flight — just future.get()
		// instead of fetching.
		bool is_leader;
		std::shared_future<CoalescedResult> future;
	};

	Ticket joinOrLead(const std::string& key);
	void complete(const std::string& key, CoalescedResult result);

private:
	struct InFlight
	{
		std::promise<CoalescedResult> promise;
		std::shared_future<CoalescedResult> future;
	};

	std::mutex mtx_;
	std::unordered_map<std::string, std::shared_ptr<InFlight>> inflight_;
};