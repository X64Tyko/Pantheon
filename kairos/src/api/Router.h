#pragma once
#include <httplib.h>
#include <memory>
#include <vector>
#include "IKairosService.h"
#include "ScheduleCache.h"

class AuthStore;
class ConfStore;
class Database;
class DownloadManager;
class EPGMaterializer;
class LogBuffer;
class RuleEngine;
class SyncManager;

class Router {
public:
	Router(httplib::Server& svr, Database& db, SyncManager& sync,
	       ConfStore& conf, LogBuffer& logs,
	       RuleEngine& engine, EPGMaterializer& materializer,
	       DownloadManager& dl, AuthStore& auth);
	~Router();
	void registerRoutes();

private:
	httplib::Server&  svr_;
	Database&         db_;
	SyncManager&      sync_;
	ConfStore&        conf_;
	LogBuffer&        logs_;
	RuleEngine&       engine_;
	EPGMaterializer&  materializer_;
	DownloadManager&  dl_;
	AuthStore&        auth_;

	ScheduleCache                              schedule_cache_;
	std::vector<std::unique_ptr<IKairosService>> services_;
};
