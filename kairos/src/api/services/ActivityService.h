#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class LogBuffer;
class SyncManager;

class ActivityService : public IKairosService {
public:
	explicit ActivityService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	SyncManager& sync_;
	LogBuffer&   logs_;
};
