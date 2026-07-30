#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class Database;

// Scheduled/manual database backup + restore. There's no separate config
// store to worry about beyond kairos.conf (see ConfStore.h) — everything
// else (all Settings-page state, source credentials aside) lives in the
// SQLite DB itself via app_config, so a DB + conf snapshot is a complete
// backup.
//
// Backup uses SQLite's online backup API (via SQLiteCpp's Backup wrapper)
// against a dedicated connection (Database::openConnection — the same
// "background thread gets its own connection" pattern SyncManager uses) so
// it's safe to run against a live, actively-written database.
//
// Restore is NOT a live hot-swap — there's no safe way to replace a SQLite
// file out from under an open connection without risking the live session's
// cached pages going stale. restore() stages the chosen backup's files
// alongside the live ones, atomically renames them over the originals, then
// calls std::exit(0); the deployed docker-compose.yml's `restart:
// unless-stopped` brings Kairos back up against the restored files. Callers
// (see BackupService) must respond to the triggering HTTP request *before*
// calling restore(), since the process won't survive past it.
class BackupManager
{
public:
	// db_path/conf_path: the live files to snapshot — the same paths passed
	// via --db/--conf (main.cpp). backup_dir: where timestamped backup pairs
	// are written; created on first use if missing.
	BackupManager(Database& db, std::string db_path, std::string conf_path, std::string backup_dir);

	struct BackupInfo
	{
		std::string id; // epoch-ms string; also the shared filename stem
		int64_t created_ms;
		int64_t size_bytes; // db + conf combined
	};

	// Oldest-first (pruneOldest relies on this order).
	std::vector<BackupInfo> list() const;

	bool isRunning() const { return running_.load(); }

	// Guarded async trigger — same shape as ContentService::
	// triggerWritebackAll. max_count: retention, applied after a successful
	// run (0 = unlimited, no pruning). Returns false if a backup is already
	// running.
	bool triggerBackup(int max_count);

	// Deletes one backup's files. Returns false if neither existed.
	bool remove(const std::string& id);

	// Stages `id`'s files over the live db/conf (after one more synchronous
	// safety backup of current state) and calls std::exit(0) on success —
	// never returns in that case. Returns false without exiting if `id`
	// doesn't exist or staging fails.
	bool restore(const std::string& id);

private:
	void runBackup(int max_count);
	void pruneOldest(int max_count);

	Database& db_;
	std::string db_path_;
	std::string conf_path_;
	std::string backup_dir_;
	std::atomic<bool> running_{false};
};