#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class ConfStore;
class Database;
class SyncManager;

class ConfigService : public IKairosService {
public:
	explicit ConfigService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&    db_;
	ConfStore&   conf_;
	SyncManager& sync_;
};
