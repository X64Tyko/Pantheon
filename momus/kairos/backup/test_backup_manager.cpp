#include <gtest/gtest.h>
#include "backup/BackupManager.h"
#include "db/ConfigRepository.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// Restore()'s success path calls std::exit() by design (see BackupManager.h's
// own comment on why a live hot-swap isn't safe) — that path is intentionally
// NOT covered here, since it would terminate the test process. Only its
// early-return failure path (nonexistent id) is tested; the rest is a manual/
// integration check (see the feature's implementation plan).
namespace
{
	std::string randomSuffix()
	{
		std::random_device rd;
		std::mt19937_64 gen(rd());
		std::uniform_int_distribution<uint64_t> dist;
		char buf[17];
		std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(dist(gen)));
		return buf;
	}
} // namespace

class BackupManagerTest : public ::testing::Test
{
protected:
	fs::path work_dir      = fs::temp_directory_path() / ("momus_backup_test_" + randomSuffix());
	std::string db_path    = (work_dir / "kairos.db").string();
	std::string conf_path  = (work_dir / "kairos.conf").string();
	std::string backup_dir = (work_dir / "backups").string();

	std::unique_ptr<Database> db;
	std::unique_ptr<BackupManager> backups;

	void SetUp() override
	{
		fs::create_directories(work_dir);
		db = std::make_unique<Database>(db_path);
		// A known row to verify the backed-up DB actually round-trips real data,
		// not just an empty schema.
		ConfigRepository(*db).setValue("backup_test_marker", "hello-world");

		std::ofstream conf(conf_path);
		conf << "[test]\ntoken = abc123\n";
		conf.close();

		backups = std::make_unique<BackupManager>(*db, db_path, conf_path, backup_dir);
	}

	void TearDown() override
	{
		db.reset();
		std::error_code ec;
		fs::remove_all(work_dir, ec);
	}

	// triggerBackup() is deliberately async (see BackupManager.h) — same
	// "real background thread, short poll, generous timeout" posture
	// JobSchedulerTest's real-thread test uses, rather than exposing a
	// test-only synchronous path just for this.
	void waitForBackupToFinish()
	{
		for (int i = 0; i < 200 && backups->isRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
		ASSERT_FALSE(backups->isRunning());
	}
};

TEST_F(BackupManagerTest, ListReturnsEmptyWhenBackupDirDoesNotExistYet)
{
	EXPECT_TRUE(backups->list().empty());
}

TEST_F(BackupManagerTest, TriggerBackupCreatesFilesListedByList)
{
	EXPECT_TRUE(backups->triggerBackup(0));
	waitForBackupToFinish();

	auto list = backups->list();
	ASSERT_EQ(list.size(), 1u);
	EXPECT_GT(list[0].size_bytes, 0);
	EXPECT_GT(list[0].created_ms, 0);
}

TEST_F(BackupManagerTest, BackedUpDatabaseRoundTripsKnownRow)
{
	ASSERT_TRUE(backups->triggerBackup(0));
	waitForBackupToFinish();

	auto list = backups->list();
	ASSERT_EQ(list.size(), 1u);

	SQLite::Database reopened((fs::path(backup_dir) / ("kairos-" + list[0].id + ".db")).string(), SQLite::OPEN_READONLY);
	SQLite::Statement q(reopened, "SELECT value FROM app_config WHERE key = 'backup_test_marker'");
	ASSERT_TRUE(q.executeStep());
	EXPECT_EQ(q.getColumn(0).getString(), "hello-world");
}

TEST_F(BackupManagerTest, BackedUpConfFileMatchesLiveConf)
{
	ASSERT_TRUE(backups->triggerBackup(0));
	waitForBackupToFinish();

	auto list = backups->list();
	ASSERT_EQ(list.size(), 1u);

	std::ifstream backed_up((fs::path(backup_dir) / ("kairos-" + list[0].id + ".conf")).string());
	std::stringstream ss;
	ss << backed_up.rdbuf();
	EXPECT_NE(ss.str().find("token = abc123"), std::string::npos);
}

TEST_F(BackupManagerTest, RetentionPruningKeepsNewestNIncludingTheJustCreatedOne)
{
	fs::create_directories(backup_dir);
	auto seed = [&](const std::string& id)
	{
		std::ofstream((fs::path(backup_dir) / ("kairos-" + id + ".db")).string()) << "fake";
		std::ofstream((fs::path(backup_dir) / ("kairos-" + id + ".conf")).string()) << "fake";
	};
	seed("1000");
	seed("2000");
	seed("3000");
	ASSERT_EQ(backups->list().size(), 3u);

	// The real backup this triggers gets an id derived from the current
	// epoch-seconds — always far newer than the seeded fake ids above.
	ASSERT_TRUE(backups->triggerBackup(2));
	waitForBackupToFinish();

	auto list = backups->list(); // oldest-first
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(list[0].id, "3000"); // second-newest fake survives
	EXPECT_NE(list[1].id, "1000");
	EXPECT_NE(list[1].id, "2000");
	EXPECT_NE(list[1].id, "3000"); // the real, newest backup
}

TEST_F(BackupManagerTest, RemoveDeletesBothFilesAndReturnsFalseForUnknownId)
{
	ASSERT_TRUE(backups->triggerBackup(0));
	waitForBackupToFinish();
	auto list = backups->list();
	ASSERT_EQ(list.size(), 1u);

	EXPECT_TRUE(backups->remove(list[0].id));
	EXPECT_TRUE(backups->list().empty());
	EXPECT_FALSE(backups->remove(list[0].id)); // already gone
	EXPECT_FALSE(backups->remove("never-existed"));
}

TEST_F(BackupManagerTest, RestoreReturnsFalseForNonexistentBackupWithoutTouchingLiveFiles)
{
	EXPECT_FALSE(backups->restore("never-existed"));
	EXPECT_TRUE(fs::exists(db_path));
	EXPECT_TRUE(fs::exists(conf_path));
}