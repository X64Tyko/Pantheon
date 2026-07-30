#pragma once
#include "../db/ConfigRepository.h"
#include "../db/Database.h"
#include <string>

// Persisted schedule/enabled state for JobScheduler-driven background jobs
// (see kairos/src/jobs/JobScheduler.h) — same ConfigRepository key/value +
// "empty means unconfigured, apply the documented default" pattern
// GuestSettings.h uses, just parameterized by job name instead of one
// function per setting, since every job here shares the same shape (enabled/
// mode/interval-or-daily-time) rather than each having its own distinct
// fields the way guest settings do.
namespace job_settings
{
	struct JobConfig
	{
		bool enabled;
		std::string mode; // "interval" or "daily"
		int interval_hours;
		int daily_hour;
		int daily_minute;
	};

	// `defaults` supplies this job's out-of-the-box schedule (see JobService's
	// and BackupService's registration code for what each job defaults to);
	// only keys actually written by a prior PATCH override it.
	inline JobConfig get(Database& db, const std::string& job_name, const JobConfig& defaults)
	{
		ConfigRepository repo(db);
		JobConfig cfg = defaults;

		auto enabled_v = repo.getValue("job_" + job_name + "_enabled");
		if (!enabled_v.empty()) cfg.enabled = (enabled_v == "1");

		auto mode_v = repo.getValue("job_" + job_name + "_mode");
		if (mode_v == "interval" || mode_v == "daily") cfg.mode = mode_v;

		auto interval_v = repo.getValue("job_" + job_name + "_interval_hours");
		if (!interval_v.empty())
		{
			try { cfg.interval_hours = std::stoi(interval_v); }
			catch (...)
			{
			}
		}

		auto hour_v = repo.getValue("job_" + job_name + "_daily_hour");
		if (!hour_v.empty())
		{
			try { cfg.daily_hour = std::stoi(hour_v); }
			catch (...)
			{
			}
		}

		auto minute_v = repo.getValue("job_" + job_name + "_daily_minute");
		if (!minute_v.empty())
		{
			try { cfg.daily_minute = std::stoi(minute_v); }
			catch (...)
			{
			}
		}

		return cfg;
	}

	inline void setEnabled(Database& db, const std::string& job_name, bool enabled)
	{
		ConfigRepository(db).setValue("job_" + job_name + "_enabled", enabled ? "1" : "0");
	}

	inline void setInterval(Database& db, const std::string& job_name, int hours)
	{
		ConfigRepository repo(db);
		repo.setValue("job_" + job_name + "_mode", "interval");
		repo.setValue("job_" + job_name + "_interval_hours", std::to_string(hours));
	}

	inline void setDaily(Database& db, const std::string& job_name, int hour, int minute)
	{
		ConfigRepository repo(db);
		repo.setValue("job_" + job_name + "_mode", "daily");
		repo.setValue("job_" + job_name + "_daily_hour", std::to_string(hour));
		repo.setValue("job_" + job_name + "_daily_minute", std::to_string(minute));
	}

	// Scheduled-backup retention — how many backup files to keep before
	// BackupManager prunes the oldest. Lives here rather than in a
	// backup-specific settings header since it's one more small persisted
	// int, same shape as everything else above.
	inline int backupMaxCount(Database& db)
	{
		auto v = ConfigRepository(db).getValue("backup_max_count");
		if (v.empty()) return 14;
		try { return std::stoi(v); }
		catch (...) { return 14; }
	}

	inline void setBackupMaxCount(Database& db, int n)
	{
		ConfigRepository(db).setValue("backup_max_count", std::to_string(n));
	}
} // namespace job_settings