#include "RequestCoalescer.h"

RequestCoalescer::Ticket RequestCoalescer::joinOrLead(const std::string& key)
{
	std::lock_guard<std::mutex> lock(mtx_);
	auto it = inflight_.find(key);
	if (it != inflight_.end())
	{
		return {false, it->second->future};
	}
	auto in        = std::make_shared<InFlight>();
	in->future     = in->promise.get_future().share();
	auto fut       = in->future;
	inflight_[key] = in;
	return {true, fut};
}

void RequestCoalescer::complete(const std::string& key, CoalescedResult result)
{
	std::shared_ptr<InFlight> in;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		auto it = inflight_.find(key);
		if (it == inflight_.end()) return; // shouldn't happen — only the leader calls complete()
		in = it->second;
		inflight_.erase(it); // new requests after this point start a fresh fetch
	}
	in->promise.set_value(std::move(result));
}