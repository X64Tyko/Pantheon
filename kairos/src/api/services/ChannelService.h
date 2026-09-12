#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"
#include "../../util/RateLimiter.h"

class Database;
class ConfStore;
class EPGMaterializer;
class LogBuffer;
class ScheduleCache;

class ChannelService : public IKairosService
{
public:
	explicit ChannelService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database& db_;
	ConfStore& conf_;
	EPGMaterializer& materializer_;
	ScheduleCache& schedule_cache_;
	LogBuffer& logs_;
	RateLimiter& guest_mutation_limiter_;
};