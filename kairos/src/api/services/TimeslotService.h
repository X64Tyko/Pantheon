#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"
#include "../../util/RateLimiter.h"

class Database;

class TimeslotService : public IKairosService
{
public:
	explicit TimeslotService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database& db_;
	RateLimiter& guest_mutation_limiter_;
};