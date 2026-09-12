#include "BackupService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../ServiceContext.h"
#include "../../backup/BackupManager.h"
#include "../../conf/JobSettings.h"
#include "../../jobs/JobScheduler.h"
#include "thread/TaskRegistry.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

BackupService::BackupService(const ServiceContext& ctx, JobScheduler& jobs, BackupManager& backups)
	: db_(ctx.db)
	, jobs_(jobs)
	, backups_(backups)
{
	// Same registration shape as JobService::registerJob, just for the one
	// job this service owns — see its header comment for why "backup" isn't
	// registered there instead. Defaults to disabled, daily at 03:00 UTC (a
	// reasonable low-traffic maintenance window).
	auto cfg = job_settings::get(db_, "backup", job_settings::JobConfig{false, "daily", 24, 3, 0});
	job_settings::setEnabled(db_, "backup", cfg.enabled);
	if (cfg.mode == "daily") job_settings::setDaily(db_, "backup", cfg.daily_hour, cfg.daily_minute);
	else job_settings::setInterval(db_, "backup", cfg.interval_hours);

	if (cfg.mode == "daily")
		jobs_.registerDaily("backup", cfg.daily_hour, cfg.daily_minute,
							[this] { backups_.triggerBackup(job_settings::backupMaxCount(db_)); });
	else
		jobs_.registerInterval("backup", std::chrono::hours(cfg.interval_hours),
							   [this] { backups_.triggerBackup(job_settings::backupMaxCount(db_)); });
	jobs_.setEnabled("backup", cfg.enabled);
}

void BackupService::registerRoutes(httplib::Server& svr)
{
	svr.Get("/api/backup", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}

		json arr  = json::array();
		auto list = backups_.list();                           // oldest-first
		for (auto it = list.rbegin(); it != list.rend(); ++it) // newest-first for display
		{
			arr.push_back(json{
				{"id", it->id},
				{"created_ms", it->created_ms},
				{"size_bytes", it->size_bytes},
			});
		}
		route::ok(res, json{
					  {"backups", arr},
					  {"max_count", job_settings::backupMaxCount(db_)},
				  }.dump());
	});

	svr.Patch("/api/backup/config", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		json body;
		try { body = json::parse(req.body); }
		catch (...)
		{
			route::err(res, 400, "invalid JSON");
			return;
		}
		if (!body.contains("max_count"))
		{
			route::err(res, 400, "max_count required");
			return;
		}
		int max_count = body.at("max_count").get<int>();
		if (max_count < 1)
		{
			route::err(res, 400, "max_count must be >= 1");
			return;
		}
		job_settings::setBackupMaxCount(db_, max_count);
		route::ok(res, "{}");
	});

	svr.Get("/api/backup/status", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		route::ok(res, json{{"running", backups_.isRunning()}}.dump());
	});

	svr.Post("/api/backup/run", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		if (!backups_.triggerBackup(job_settings::backupMaxCount(db_)))
		{
			route::err(res, 409, "backup already running");
			return;
		}
		res.status = 202;
		route::ok(res, json{{"status", "started"}}.dump());
	});

	svr.Delete("/api/backup/:id", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		if (!backups_.remove(req.path_params.at("id")))
		{
			route::err(res, 404, "backup not found");
			return;
		}
		res.status = 204;
	});

	// Stages the chosen backup over the live db/conf and restarts the
	// process (see BackupManager::restore's own comment on why this can't be
	// a live hot-swap) — docker-compose.yml's `restart: unless-stopped`
	// brings Kairos back up against the restored files. Responds before the
	// restore actually runs, then does it from a detached thread after a
	// short delay so this response has time to reach the client first; the
	// client is expected to poll /health and show a "restarting" state.
	svr.Post("/api/backup/:id/restore", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		const std::string id = req.path_params.at("id");
		bool exists          = false;
		for (const auto& b : backups_.list()) if (b.id == id)
		{
			exists = true;
			break;
		}
		if (!exists)
		{
			route::err(res, 404, "backup not found");
			return;
		}

		res.status = 202;
		route::ok(res, json{{"status", "restoring"}}.dump());

		TaskRegistry::global().spawn([this, id]()
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			backups_.restore(id); // exits the process on success
		});
	});
}