#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;
class ConfStore;
class LogBuffer;
class ScheduleCache;

class ChannelService : public IKairosService {
public:
	explicit ChannelService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&      db_;
	ConfStore&     conf_;
	ScheduleCache& schedule_cache_;
	LogBuffer&     logs_;
};
