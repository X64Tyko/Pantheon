#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;
class SyncManager;

class FillerService : public IKairosService {
public:
	explicit FillerService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&    db_;
	SyncManager& sync_;
};
