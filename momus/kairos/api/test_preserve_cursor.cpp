// Regression coverage for the ?preserve_cursor=true escape hatch on the
// channel/block mutation routes that otherwise hard-reset a channel's
// accumulated cursor state (ScheduleCache::hardReset -- see ChannelService.cpp
// and BlockService.cpp). Structural edits (day_mask, seed, adding/removing a
// block, ...) wipe media_cursor/block_state by default so a stale projection
// can't be trusted; Hades' save-time "keep positions" prompt opts out of that
// per the user's own request via this param, and this file locks in that both
// the field still applies AND the cursor really does (not) survive.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "api/Router.h"
#include "auth/AuthStore.h"
#include "conf/ConfStore.h"
#include "db/CursorRepository.h"
#include "db/Database.h"
#include "download/DownloadManager.h"
#include "email/EmailService.h"
#include "log/LogBuffer.h"
#include "backup/BackupManager.h"
#include "jobs/JobScheduler.h"
#include "scheduler/CursorState.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "source/SyncManager.h"

using json = nlohmann::json;

class PreserveCursorTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ConfStore conf{"./momus_preserve_cursor_test.conf"};
	SyncManager sync{db, conf};
	RuleEngine engine{db};
	EPGMaterializer materializer{db, engine};
	DownloadManager dl;
	AuthStore auth{db};
	EmailService email{db};
	LogBuffer logs;
	JobScheduler jobs;
	BackupManager backups{db, "", "", "/tmp/kairos_test_backups_unused"};

	httplib::Server svr;
	std::unique_ptr<Router> router;
	std::unique_ptr<httplib::Client> cli;
	std::thread server_thread;

	std::string admin_token;

	void SetUp() override
	{
		router = std::make_unique<Router>(svr, db, sync, conf, logs, engine, materializer, dl, auth, email, jobs, backups);
		router->registerRoutes();

		int port      = svr.bind_to_any_port("127.0.0.1");
		server_thread = std::thread([this] { svr.listen_after_bind(); });
		for (int i = 0; i < 200 && !svr.is_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));

		cli = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(port));
		cli->set_connection_timeout(5);
		cli->set_read_timeout(5);

		auth.createUser("pcur_admin", "pcur-password-1", "admin");
		admin_token = auth.login("pcur_admin", "pcur-password-1");

		SQLite::Statement c(db.get(),
							"INSERT INTO channel (channel_id, name, number, seed) VALUES ('c1','Ch',1,111)");
		c.exec();
		SQLite::Statement b(db.get(),
							"INSERT INTO block (block_id, channel_id, name, block_type, day_mask, start_time) "
							"VALUES ('b1','c1','Block','episode',127,'00:00')");
		b.exec();
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
		std::error_code ec;
		std::filesystem::remove("./momus_preserve_cursor_test.conf", ec);
	}

	httplib::Headers headers() const { return {{"Authorization", "Bearer " + admin_token}}; }

	// Plants a block-scoped cursor for c1/b1 so hardReset has something to
	// wipe. content_type "movie" with no episode_id sidesteps media_cursor's
	// FK on episode(episode_id) -- this test only cares whether the row
	// survives, not real playback-position semantics.
	void plantCursor()
	{
		CursorState st = CursorState::loadFromDB(db, "c1");
		st.setCursorPos("movie", "movie1", "block", "b1", 3);
		st.applyToDB(db, "c1");
	}

	bool cursorSurvived()
	{
		CursorState st = CursorState::loadFromDB(db, "c1");
		return st.hasCursor("movie", "movie1", "block", "b1");
	}
};

// ---------------------------------------------------------------------------
// PATCH /api/channels/:id/blocks/:bid -- structural field
// ---------------------------------------------------------------------------

TEST_F(PreserveCursorTest, StructuralBlockPatchWipesCursorByDefault)
{
	plantCursor();
	ASSERT_TRUE(cursorSurvived());

	auto r = cli->Patch("/api/channels/c1/blocks/b1", headers(), R"({"day_mask":1})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_FALSE(cursorSurvived());
}

TEST_F(PreserveCursorTest, StructuralBlockPatchWithPreserveCursorKeepsCursor)
{
	plantCursor();
	ASSERT_TRUE(cursorSurvived());

	auto r = cli->Patch("/api/channels/c1/blocks/b1?preserve_cursor=true", headers(),
						R"({"day_mask":1})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_TRUE(cursorSurvived());

	// The field itself still applies -- this isn't a silent no-op.
	auto list = cli->Get("/api/channels/c1/blocks", headers());
	ASSERT_TRUE(list);
	json blocks = json::parse(list->body);
	ASSERT_EQ(blocks.size(), 1);
	EXPECT_EQ(blocks[0]["day_mask"], 1);
}

TEST_F(PreserveCursorTest, NonStructuralBlockPatchNeverTouchesCursorEitherWay)
{
	plantCursor();
	auto r = cli->Patch("/api/channels/c1/blocks/b1", headers(), R"({"program_count":5})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_TRUE(cursorSurvived());
}

// ---------------------------------------------------------------------------
// POST / DELETE /api/channels/:id/blocks -- new/removed block
// ---------------------------------------------------------------------------

TEST_F(PreserveCursorTest, CreateBlockWipesCursorByDefault)
{
	plantCursor();
	auto r = cli->Post("/api/channels/c1/blocks", headers(), R"({"name":"New"})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 201);
	EXPECT_FALSE(cursorSurvived());
}

TEST_F(PreserveCursorTest, CreateBlockWithPreserveCursorKeepsCursor)
{
	plantCursor();
	auto r = cli->Post("/api/channels/c1/blocks?preserve_cursor=true", headers(), R"({"name":"New"})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 201);
	EXPECT_TRUE(cursorSurvived());
}

TEST_F(PreserveCursorTest, DeleteBlockWithPreserveCursorKeepsCursor)
{
	SQLite::Statement b2(db.get(),
						 "INSERT INTO block (block_id, channel_id, name, block_type, day_mask, start_time) "
						 "VALUES ('b2','c1','Doomed','episode',127,'01:00')");
	b2.exec();
	plantCursor();

	auto r = cli->Delete("/api/channels/c1/blocks/b2?preserve_cursor=true", headers());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_TRUE(cursorSurvived());
}

// ---------------------------------------------------------------------------
// PATCH /api/channels/:id -- seed change
// ---------------------------------------------------------------------------

TEST_F(PreserveCursorTest, SeedChangeWipesCursorByDefault)
{
	plantCursor();
	auto r = cli->Patch("/api/channels/c1", headers(), R"({"seed":222})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_FALSE(cursorSurvived());
}

TEST_F(PreserveCursorTest, SeedChangeWithPreserveCursorKeepsCursor)
{
	plantCursor();
	auto r = cli->Patch("/api/channels/c1?preserve_cursor=true", headers(), R"({"seed":222})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_TRUE(cursorSurvived());

	auto list = cli->Get("/api/channels", headers());
	ASSERT_TRUE(list);
	json chans = json::parse(list->body);
	ASSERT_EQ(chans.size(), 1);
	EXPECT_EQ(chans[0]["seed"], 222);
}