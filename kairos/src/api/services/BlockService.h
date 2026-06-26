#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;
class ScheduleCache;
class ConfStore;

class BlockService : public IKairosService {
public:
	explicit BlockService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&      db_;
	ScheduleCache& schedule_cache_;
	ConfStore&     conf_;
};
