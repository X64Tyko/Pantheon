#pragma once
#include "../IKairosService.h"
#include <mutex>
#include <string>
#include <ctime>

// GET /api/about — public, unauthenticated. Serves the "About Pantheon"
// markdown (docs/About.md) fetched live from GitHub and cached in memory, so
// the in-app page and the published docs site (x64tyko.github.io/Pantheon/
// About.html) can never drift out of sync with each other — there's exactly
// one source of truth, this file isn't it.
class AboutService : public IKairosService
{
public:
	AboutService() = default;
	void registerRoutes(httplib::Server& svr) override;

private:
	// Returns the cached markdown, refreshing it first if the cache is stale.
	// A refresh failure (GitHub unreachable, rate-limited, etc.) falls back to
	// whatever was last cached rather than failing the request — the whole
	// point of caching here is that a transient GitHub outage shouldn't take
	// the About page down for every single visitor.
	std::string getContent();

	std::mutex mutex_;
	std::string cached_content_;
	std::time_t cached_at_ = 0;
};