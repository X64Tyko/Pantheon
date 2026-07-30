#pragma once
#include "../IKairosService.h"
#include "../../conf/JobSettings.h"
#include <httplib.h>
#include <string>

class ChapterDetectionManager;
class ContentService;
class Database;
class JobScheduler;
class ScraperManager;
class SyncManager;
struct ServiceContext;

// Owns scheduling for the four content-pipeline jobs (sync, metadata_refresh,
// chapter_detection, writeback_sweep) — each just wraps an existing
// async/single-flight-guarded trigger*() method Router already has a
// reference to. The fifth scheduled job, "backup", is registered by
// BackupService instead (it needs BackupManager, which Router doesn't hand
// to this service) but lives in the same shared JobScheduler instance, so
// GET/PATCH /api/jobs below — which just reads/writes JobScheduler +
// JobSettings by name — covers it too without this class knowing anything
// backup-specific. Only POST /api/jobs/:name/run-now is scoped to the four
// jobs this service owns; backup's manual trigger is POST /api/backup/run.
class JobService : public IKairosService
{
public:
	JobService(const ServiceContext& ctx, JobScheduler& jobs, SyncManager& sync,
			   ScraperManager& scraper, ChapterDetectionManager& chapters,
			   ContentService& content);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database& db_;
	JobScheduler& jobs_;
	SyncManager& sync_;
	ScraperManager& scraper_;
	ChapterDetectionManager& chapters_;
	ContentService& content_;

	// Registers one job against jobs_, seeded from job_settings (falling back
	// to `fallback` on first-ever boot) and immediately writing the resolved
	// config back to job_settings — so GET /api/jobs can trust the DB alone
	// for display without re-deriving each job's default a second time.
	void registerJob(const std::string& name, const job_settings::JobConfig& fallback);

	// Runs job `name`'s underlying operation right now, bypassing the
	// schedule. Returns false if it was already running (a no-op, not an
	// error) — used by both each job's scheduled callback (return value
	// ignored there) and POST .../run-now (surfaced to the caller).
	bool runNow(const std::string& name);
};