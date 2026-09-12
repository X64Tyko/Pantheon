#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"
#include "../../util/RateLimiter.h"

class AuthStore;
class Database;
class EmailService;

class AuthService : public IKairosService
{
public:
	explicit AuthService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	AuthStore& auth_;
	Database& db_;
	EmailService& email_;

	// Per-IP throttle on POST /api/auth/guest — unlike login, a not-yet-
	// created guest account has no row of its own to attach a fail-counter
	// to, so this has to live at the route/service layer instead of
	// AuthStore. See RateLimiter.h.
	RateLimiter guest_creation_limiter_;
};