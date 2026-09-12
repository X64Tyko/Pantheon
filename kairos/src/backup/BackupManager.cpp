#include "BackupManager.h"
#include "../db/Database.h"
#include "thread/TaskRegistry.h"
#include <SQLiteCpp/Backup.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
#include <thread>

namespace fs = std::filesystem;

namespace
{
	constexpr const char* kPrefix = "kairos-";

	std::string dbFile(const std::string& dir, const std::string& id) { return dir + "/" + kPrefix + id + ".db"; }
	std::string confFile(const std::string& dir, const std::string& id) { return dir + "/" + kPrefix + id + ".conf"; }
} // namespace

BackupManager::BackupManager(Database& db, std::string db_path, std::string conf_path, std::string backup_dir)
	: db_(db)
	, db_path_(std::move(db_path))
	, conf_path_(std::move(conf_path))
	, backup_dir_(std::move(backup_dir))
{
}

std::vector<BackupManager::BackupInfo> BackupManager::list() const
{
	std::vector<BackupInfo> out;
	std::error_code ec;
	if (!fs::exists(backup_dir_, ec)) return out;

	// Each backup is a .db + .conf pair sharing an id — group by id and sum
	// their sizes into one entry.
	std::map<std::string, int64_t> size_by_id;
	for (const auto& entry : fs::directory_iterator(backup_dir_, ec))
	{
		if (ec || !entry.is_regular_file()) continue;
		const std::string name = entry.path().filename().string();
		if (name.rfind(kPrefix, 0) != 0) continue;
		const std::string rest = name.substr(std::string(kPrefix).size());
		const auto dot         = rest.find('.');
		if (dot == std::string::npos) continue;
		const std::string id  = rest.substr(0, dot);
		const std::string ext = rest.substr(dot);
		if (ext != ".db" && ext != ".conf") continue; // skip .restoring staging files, if any linger
		std::error_code size_ec;
		size_by_id[id] += static_cast<int64_t>(entry.file_size(size_ec));
	}

	out.reserve(size_by_id.size());
	for (const auto& [id, size] : size_by_id)
	{
		int64_t created_ms = 0;
		try { created_ms = std::stoll(id); }
		catch (...)
		{
		}
		out.push_back(BackupInfo{id, created_ms, size});
	}
	std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.created_ms < b.created_ms; });
	return out;
}

bool BackupManager::triggerBackup(int max_count)
{
	bool expected = false;
	if (!running_.compare_exchange_strong(expected, true)) return false;

	TaskRegistry::global().spawn([this, max_count]()
	{
		try
		{
			runBackup(max_count);
		}
		catch (const std::exception& e)
		{
			std::cerr << "[backup] error: " << e.what() << std::endl;
		}
		running_.store(false);
	});
	return true;
}

void BackupManager::runBackup(int max_count)
{
	std::error_code ec;
	fs::create_directories(backup_dir_, ec);

	const std::string id       = std::to_string(static_cast<int64_t>(std::time(nullptr)) * 1000);
	const std::string db_out   = dbFile(backup_dir_, id);
	const std::string conf_out = confFile(backup_dir_, id);

	try
	{
		SQLite::Database src(db_.openConnection(60000));
		SQLite::Database dest(db_out, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
		SQLite::Backup backup(dest, src);

		// Reference implementation from sqlite.org/backup.html: retry on
		// BUSY/LOCKED with a short backoff instead of busy-spinning, since a
		// live write transaction on the source can transiently hold either.
		int rc;
		do
		{
			rc = backup.executeStep(100);
			if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) std::this_thread::sleep_for(std::chrono::milliseconds(250));
		} while (rc != SQLITE_DONE);

		fs::copy_file(conf_path_, conf_out, fs::copy_options::overwrite_existing, ec);
	}
	catch (const std::exception& e)
	{
		std::cerr << "[backup] failed: " << e.what() << std::endl;
		fs::remove(db_out, ec);
		fs::remove(conf_out, ec);
		return;
	}

	std::cout << "[backup] wrote " << db_out << std::endl;
	if (max_count > 0) pruneOldest(max_count);
}

void BackupManager::pruneOldest(int max_count)
{
	auto backups = list(); // oldest-first
	if (static_cast<int>(backups.size()) <= max_count) return;
	const size_t to_remove = backups.size() - static_cast<size_t>(max_count);
	for (size_t i = 0; i < to_remove; ++i)
	{
		std::cout << "[backup] pruning old backup " << backups[i].id << std::endl;
		remove(backups[i].id);
	}
}

bool BackupManager::remove(const std::string& id)
{
	std::error_code ec;
	const bool a = fs::remove(dbFile(backup_dir_, id), ec);
	const bool b = fs::remove(confFile(backup_dir_, id), ec);
	return a || b;
}

bool BackupManager::restore(const std::string& id)
{
	const std::string src_db   = dbFile(backup_dir_, id);
	const std::string src_conf = confFile(backup_dir_, id);
	std::error_code ec;
	if (!fs::exists(src_db, ec)) return false;

	// One more safety net before overwriting live state — synchronous, not
	// triggerBackup()'s async form: the process is about to exit, so there's
	// no "later" for a background thread to finish in. Not counted against
	// retention (max_count=0) since it's a one-off, not part of the rotation.
	runBackup(0);

	const std::string staged_db   = db_path_ + ".restoring";
	const std::string staged_conf = conf_path_ + ".restoring";

	fs::copy_file(src_db, staged_db, fs::copy_options::overwrite_existing, ec);
	if (ec)
	{
		std::cerr << "[backup] restore stage (db) failed: " << ec.message() << std::endl;
		return false;
	}
	if (fs::exists(src_conf, ec))
	{
		fs::copy_file(src_conf, staged_conf, fs::copy_options::overwrite_existing, ec);
	}

	std::cout << "[backup] restoring from '" << id << "' — exiting for restart" << std::endl;
	std::cout.flush();

	// Same-directory rename (both db_path_ and its .restoring sibling live
	// alongside each other under /data) — atomic, and safe even though this
	// process's own connection still has the old file open: the rename
	// doesn't touch that open fd, only the directory entry a fresh process
	// will resolve on next start.
	fs::rename(staged_db, db_path_, ec);
	if (ec)
	{
		std::cerr << "[backup] restore rename (db) failed: " << ec.message() << std::endl;
		return false;
	}
	if (fs::exists(staged_conf, ec))
	{
		fs::rename(staged_conf, conf_path_, ec);
	}

	std::exit(0);
}