#pragma once
#include <httplib.h>
#include <memory>
#include <vector>
#include "IKairosService.h"
#include "ScheduleCache.h"
#include "../scheduler/EPGDivergenceChecker.h"
#include "../util/RateLimiter.h"

class AuthStore;
class BackupManager;
class ChapterDetectionManager;
class ConfStore;
class ContentService;
class Database;
class DownloadManager;
class EmailService;
class EPGMaterializer;
class JobScheduler;
class LogBuffer;
class MediaNormalizeManager;
class RuleEngine;
class ScraperManager;
class SyncManager;

class Router
{
public:
	Router(httplib::Server& svr, Database& db, SyncManager& sync,
		   ConfStore& conf, LogBuffer& logs,
		   RuleEngine& engine, EPGMaterializer& materializer,
		   DownloadManager& dl, AuthStore& auth, EmailService& email,
		   JobScheduler& jobs, BackupManager& backups);
	~Router();
	void registerRoutes();

private:
	httplib::Server& svr_;
	Database& db_;
	SyncManager& sync_;
	ConfStore& conf_;
	LogBuffer& logs_;
	RuleEngine& engine_;
	EPGMaterializer& materializer_;
	DownloadManager& dl_;
	AuthStore& auth_;
	EmailService& email_;
	JobScheduler& jobs_;
	BackupManager& backups_;

	ScheduleCache schedule_cache_;
	EPGDivergenceChecker divergence_checker_;
	// Shared across ChannelService/BlockService/TimeslotService (not one
	// instance per service) so a guest/real-viewer can't multiply their
	// effective rate by spreading channel-builder calls across all three.
	RateLimiter guest_mutation_limiter_;
	std::unique_ptr<ScraperManager> scraper_mgr_;
	std::unique_ptr<ChapterDetectionManager> chapter_detect_mgr_;
	std::unique_ptr<MediaNormalizeManager> normalize_mgr_;
	// Raw, non-owning — the owning unique_ptr moves into services_ once
	// constructed (see registerRoutes()). Kept for JobService's
	// writeback_sweep job, same reason ScraperManager's match-confirmed
	// callback already needed this pointer (see registerRoutes()'s own
	// comment on it).
	ContentService* content_service_ptr_ = nullptr;
	std::vector<std::unique_ptr<IKairosService>> services_;
};