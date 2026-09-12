#pragma once
#include "../IKairosService.h"
#include <httplib.h>

class BackupManager;
class Database;
class JobScheduler;
struct ServiceContext;

// Registers the "backup" scheduled job (see JobService.h's own comment on
// why that job lives here instead of there) and owns every backup/restore
// HTTP route. Schedule config for "backup" itself (enabled/mode/interval)
// still goes through the shared GET/PATCH /api/jobs — this only adds what's
// backup-specific: the file list, retention, and restore.
class BackupService : public IKairosService
{
public:
	BackupService(const ServiceContext& ctx, JobScheduler& jobs, BackupManager& backups);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database& db_;
	JobScheduler& jobs_;
	BackupManager& backups_;
};