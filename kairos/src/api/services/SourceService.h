#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class AuthStore;
class Database;
class EmailService;
class LogBuffer;
class SyncManager;

class SourceService : public IKairosService {
public:
	explicit SourceService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&     db_;
	SyncManager&  sync_;
	LogBuffer&    logs_;
	AuthStore&    auth_;
	EmailService& email_;
};
