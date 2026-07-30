#include "JobService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../ServiceContext.h"
#include "../../db/ChapterRepository.h"
#include "../../detect/ChapterDetectionManager.h"
#include "../../jobs/JobScheduler.h"
#include "../../scraper/ScraperManager.h"
#include "../../source/SyncManager.h"
#include "ContentService.h"
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

namespace
{
	// All five scheduled jobs, including "backup" (registered by
	// BackupService, not this class — see JobService.h) — GET/PATCH
	// /api/jobs is a unified surface over whatever's actually registered in
	// the shared JobScheduler.
	constexpr std::array<const char*, 5> kKnownJobs = {
		"sync", "metadata_refresh", "chapter_detection", "writeback_sweep", "backup"
	};

	bool isKnownJob(const std::string& name)
	{
		for (const char* n : kKnownJobs) if (name == n) return true;
		return false;
	}
}

JobService::JobService(const ServiceContext& ctx, JobScheduler& jobs, SyncManager& sync,
					   ScraperManager& scraper, ChapterDetectionManager& chapters,
					   ContentService& content)
	: db_(ctx.db)
	, jobs_(jobs)
	, sync_(sync)
	, scraper_(scraper)
	, chapters_(chapters)
	, content_(content)
{
	// All four default to disabled + a daily cadence except sync, which
	// defaults to a tighter interval since it's the one job most operators
	// will actually want running regularly once they opt in.
	registerJob("sync", job_settings::JobConfig{false, "interval", 6, 3, 0});
	registerJob("metadata_refresh", job_settings::JobConfig{false, "daily", 24, 3, 0});
	registerJob("chapter_detection", job_settings::JobConfig{false, "daily", 24, 3, 0});
	registerJob("writeback_sweep", job_settings::JobConfig{false, "daily", 24, 3, 0});
}

void JobService::registerJob(const std::string& name, const job_settings::JobConfig& fallback)
{
	auto cfg = job_settings::get(db_, name, fallback);

	// Normalize straight back to job_settings so a first-ever GET /api/jobs
	// reflects exactly what got registered, without GET needing to know each
	// job's default a second time.
	job_settings::setEnabled(db_, name, cfg.enabled);
	if (cfg.mode == "daily") job_settings::setDaily(db_, name, cfg.daily_hour, cfg.daily_minute);
	else job_settings::setInterval(db_, name, cfg.interval_hours);

	if (cfg.mode == "daily") jobs_.registerDaily(name, cfg.daily_hour, cfg.daily_minute, [this, name] { runNow(name); });
	else jobs_.registerInterval(name, std::chrono::hours(cfg.interval_hours), [this, name] { runNow(name); });

	// registerInterval/registerDaily always add a job enabled — apply the
	// persisted (possibly disabled) state on top.
	jobs_.setEnabled(name, cfg.enabled);
}

bool JobService::runNow(const std::string& name)
{
	if (name == "sync")
	{
		if (sync_.isSyncing()) return false;
		sync_.triggerSync();
		return true;
	}
	if (name == "metadata_refresh")
	{
		if (scraper_.isRefreshingAll()) return false;
		scraper_.triggerRefreshAll();
		return true;
	}
	if (name == "chapter_detection")
	{
		if (chapters_.isDetecting()) return false;
		auto ids = ChapterRepository(db_).getShowIdsNeedingDetection(1);
		if (ids.empty()) return true; // nothing eligible — not an error
		return chapters_.triggerShowDetect(ids.front());
	}
	if (name == "writeback_sweep") return content_.triggerWritebackAll();
	return false;
}

void JobService::registerRoutes(httplib::Server& svr)
{
	svr.Get("/api/jobs", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}

		auto statuses = jobs_.listJobs();
		json arr      = json::array();
		for (const char* name : kKnownJobs)
		{
			// Dummy fallback: by request time, registration (JobService's
			// constructor, or BackupService's) has already run and always
			// writes through, so this is never actually used.
			auto cfg = job_settings::get(db_, name, job_settings::JobConfig{false, "daily", 24, 3, 0});

			json j{
				{"name", name},
				{"enabled", cfg.enabled},
				{"mode", cfg.mode},
				{"interval_hours", cfg.interval_hours},
				{"daily_hour", cfg.daily_hour},
				{"daily_minute", cfg.daily_minute},
				{"next_run_ms", nullptr},
				{"last_run_ms", 0},
				{"last_run_ok", true},
				{"last_error", ""},
			};
			for (const auto& s : statuses)
			{
				if (s.name != name) continue;
				j["next_run_ms"] = s.next_due_ms;
				j["last_run_ms"] = s.last_run_ms;
				j["last_run_ok"] = s.last_run_ok;
				j["last_error"]  = s.last_error;
				break;
			}
			arr.push_back(std::move(j));
		}
		route::ok(res, arr.dump());
	});

	svr.Patch("/api/jobs/:name", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		const std::string name = req.path_params.at("name");
		if (!isKnownJob(name))
		{
			route::err(res, 404, "unknown job");
			return;
		}

		json body;
		try { body = json::parse(req.body); }
		catch (...)
		{
			route::err(res, 400, "invalid JSON");
			return;
		}

		if (body.contains("enabled"))
		{
			bool enabled = body.at("enabled").get<bool>();
			job_settings::setEnabled(db_, name, enabled);
			jobs_.setEnabled(name, enabled);
		}

		const std::string mode = body.value("mode", "");
		if (mode == "interval")
		{
			int hours = body.value("interval_hours", 0);
			if (hours < 1)
			{
				route::err(res, 400, "interval_hours must be >= 1");
				return;
			}
			job_settings::setInterval(db_, name, hours);
			jobs_.setInterval(name, std::chrono::hours(hours));
		}
		else if (mode == "daily")
		{
			int hour   = body.value("daily_hour", -1);
			int minute = body.value("daily_minute", -1);
			if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
			{
				route::err(res, 400, "daily_hour/daily_minute out of range");
				return;
			}
			job_settings::setDaily(db_, name, hour, minute);
			jobs_.setDaily(name, hour, minute);
		}
		else if (!mode.empty())
		{
			route::err(res, 400, "mode must be 'interval' or 'daily'");
			return;
		}

		route::ok(res, "{}");
	});

	svr.Post("/api/jobs/:name/run-now", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		const std::string name = req.path_params.at("name");
		if (name == "backup")
		{
			route::err(res, 400, "use POST /api/backup/run for the backup job");
			return;
		}
		if (!isKnownJob(name))
		{
			route::err(res, 404, "unknown job");
			return;
		}

		res.status = 202;
		route::ok(res, json{{"status", runNow(name) ? "started" : "already_running"}}.dump());
	});
}