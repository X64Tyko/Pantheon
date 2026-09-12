#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class ConfStore;
class Database;
class LogBuffer;
class SyncManager;

class ActivityService : public IKairosService
{
public:
	explicit ActivityService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database& db_;
	SyncManager& sync_;
	LogBuffer& logs_;
	ConfStore& conf_;
};