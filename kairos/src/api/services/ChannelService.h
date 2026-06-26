#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;
class ConfStore;
class ScheduleCache;

class ChannelService : public IKairosService {
public:
	explicit ChannelService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&      db_;
	ConfStore&     conf_;
	ScheduleCache& schedule_cache_;
};
