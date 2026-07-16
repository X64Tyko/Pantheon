#pragma once
#include <httplib.h>
#include <memory>
#include <vector>
#include "IKairosService.h"
#include "ScheduleCache.h"
#include "../scheduler/EPGDivergenceChecker.h"

class AuthStore;
class ChapterDetectionManager;
class ConfStore;
class Database;
class DownloadManager;
class EmailService;
class EPGMaterializer;
class LogBuffer;
class RuleEngine;
class ScraperManager;
class SyncManager;

class Router {
public:
	Router(httplib::Server& svr, Database& db, SyncManager& sync,
	       ConfStore& conf, LogBuffer& logs,
	       RuleEngine& engine, EPGMaterializer& materializer,
	       DownloadManager& dl, AuthStore& auth, EmailService& email);
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
	EmailService&     email_;

	ScheduleCache                                schedule_cache_;
	EPGDivergenceChecker                         divergence_checker_;
	std::unique_ptr<ScraperManager>              scraper_mgr_;
	std::unique_ptr<ChapterDetectionManager>     chapter_detect_mgr_;
	std::vector<std::unique_ptr<IKairosService>> services_;
};
