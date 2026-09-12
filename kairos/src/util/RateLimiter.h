#pragma once
#include <httplib.h>
#include <chrono>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

// Small fixed-window per-key rate limiter — e.g. throttling guest-account
// creation per client IP, which (unlike login) has no account row of its own
// to attach a fail-counter/lockout to before one exists. Deliberately simple
// (in-memory, process-lifetime, fixed rather than sliding window) since this
// is meant to blunt scripted abuse, not provide precise accounting; a full
// token-bucket/sliding-window implementation would be overkill for the one
// or two call sites that need this.
class RateLimiter
{
public:
	// Returns true and counts this call against `key` if it's still under
	// `max_count` within the current `window_seconds` window; false (and
	// leaves the count unchanged) once the window's cap is reached. Callers
	// should reject the request when this returns false.
	bool allow(const std::string& key, int max_count, int64_t window_seconds)
	{
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard<std::mutex> lock(mtx_);

		// Unbounded growth guard for a long-running process: if the map has
		// grown large, drop entries whose window has already expired before
		// doing anything else. Cheap relative to how rarely this path is hit
		// (only once the map is already big), and keeps memory bounded
		// without needing a separate background sweep thread.
		if (buckets_.size() > 10'000)
		{
			for (auto it = buckets_.begin(); it != buckets_.end();)
			{
				it = (now - it->second.window_start) > std::chrono::seconds(window_seconds)
						 ? buckets_.erase(it)
						 : std::next(it);
			}
		}

		auto& bucket = buckets_[key];
		if (now - bucket.window_start > std::chrono::seconds(window_seconds))
		{
			bucket.window_start = now;
			bucket.count        = 0;
		}
		if (bucket.count >= max_count) return false;
		++bucket.count;
		return true;
	}

private:
	struct Bucket
	{
		std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
		int count                                          = 0;
	};

	std::mutex mtx_;
	std::unordered_map<std::string, Bucket> buckets_;
};

// Best-effort real client address: the leftmost hop in X-Forwarded-For (the
// original client, per standard XFF chaining — see Hermes's
// appendForwardedFor) if present, else whatever this process saw directly on
// its own socket. The header is only trustworthy because Kairos is meant to
// be reached solely through Hermes in a public deployment (see
// docker-compose.yml's port-8081 note) — it's a best-effort abuse-throttling
// signal, not an auth decision, so a spoofed value merely lets one attacker
// evade their own rate limit rather than impersonate or access anything.
inline std::string clientAddressForRateLimit(const httplib::Request& req)
{
	std::string xff = req.get_header_value("X-Forwarded-For");
	if (xff.empty()) return req.remote_addr;
	auto comma = xff.find(',');
	return comma == std::string::npos ? xff : xff.substr(0, comma);
}