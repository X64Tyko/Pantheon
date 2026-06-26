#pragma once
#include "../IKairosService.h"

struct ServiceContext;
class ConfStore;
class Database;
class EPGMaterializer;
class RuleEngine;
class ScheduleCache;

class SchedulerService : public IKairosService {
public:
	explicit SchedulerService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&        db_;
	ConfStore&       conf_;
	RuleEngine&      engine_;
	EPGMaterializer& materializer_;
	ScheduleCache&   schedule_cache_;
};
