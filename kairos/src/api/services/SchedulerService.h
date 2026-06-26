#pragma once
#include "../IKairosService.h"
#include <nlohmann/json_fwd.hpp>

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
	void attachSourceMapping(nlohmann::json& j, const std::string& item_id);

	Database&        db_;
	ConfStore&       conf_;
	RuleEngine&      engine_;
	EPGMaterializer& materializer_;
	ScheduleCache&   schedule_cache_;
};
