#include "AboutService.h"
#include "../RouteHelpers.h"
#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

namespace
{
	constexpr const char* kHost = "https://raw.githubusercontent.com";
	constexpr const char* kPath = "/X64Tyko/Pantheon/master/docs/About.md";
	// Content changes rarely (a doc edit, not a live setting) — an hour keeps
	// the About page reasonably fresh without hammering GitHub on every visit.
	constexpr std::time_t kCacheTtlSeconds = 3600;
}

void AboutService::registerRoutes(httplib::Server& svr)
{
	svr.Get("/api/about", [this](const Req&, Res& res)
	{
		route::ok(res, json{{"content", getContent()}}.dump());
	});
}

std::string AboutService::getContent()
{
	std::lock_guard<std::mutex> lock(mutex_);

	std::time_t now = std::time(nullptr);
	if (!cached_content_.empty() && (now - cached_at_) < kCacheTtlSeconds) return cached_content_;

	httplib::Client client(kHost);
	client.set_connection_timeout(5);
	client.set_read_timeout(10);
	client.set_follow_location(true);

	auto result = client.Get(kPath);
	if (result && result->status == 200 && !result->body.empty())
	{
		cached_content_ = result->body;
		cached_at_      = now;
	}
	// On failure, fall through and return whatever's cached (possibly stale,
	// possibly still empty on a cold start with no prior successful fetch) —
	// a GitHub hiccup shouldn't take the About page down for every visitor.
	return cached_content_;
}