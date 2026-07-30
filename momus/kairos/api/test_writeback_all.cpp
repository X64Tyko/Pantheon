// Endpoint-level tests for POST /api/writeback/all and GET /api/writeback/status
// — spins up a REAL Router (same wiring as main.cpp), same fixture shape as
// test_content_service_security.cpp. Focus is auth gating (both routes are
// admin-only, matching the single-item writeback routes) and that a trigger
// round-trips end-to-end (202 → eventually running:false) without asserting
// on push semantics, which are covered by source/test_jellyfin_source.cpp's
// PushMetadata_* tests and db/test_content_repository_writeback.cpp's
// eligible-ids filtering tests. A deliberately-not-included case: forcing a
// real 409 "already running" collision is too timing-dependent to assert on
// without an injection point the production code has no other reason to
// have — the compare_exchange guard itself is simple enough to trust by
// inspection here.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

#include "api/Router.h"
#include "auth/AuthStore.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include "download/DownloadManager.h"
#include "email/EmailService.h"
#include "log/LogBuffer.h"
#include "backup/BackupManager.h"
#include "jobs/JobScheduler.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "source/SyncManager.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

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
}

class WritebackAllTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	std::string conf_path = (fs::temp_directory_path() / ("momus_writeback_conf_" + randomSuffix())).string();
	ConfStore conf{conf_path};
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

	std::string admin_token, viewer_token;

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

		auth.createUser("wbtest_admin", "wbtest-password-1", "admin");
		auth.createUser("wbtest_viewer", "wbtest-password-2", "viewer");
		admin_token  = auth.login("wbtest_admin", "wbtest-password-1");
		viewer_token = auth.login("wbtest_viewer", "wbtest-password-2");
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
		std::error_code ec;
		fs::remove(conf_path, ec);
	}

	httplib::Headers adminHeaders() const { return {{"Authorization", "Bearer " + admin_token}}; }
	httplib::Headers viewerHeaders() const { return {{"Authorization", "Bearer " + viewer_token}}; }
};

TEST_F(WritebackAllTest, PostWritebackAll_RejectsUnauthenticated)
{
	// 401 (no/invalid credentials) here, vs 403 (authenticated, wrong role)
	// for the viewer case below — a pre-routing auth filter rejects the
	// missing-token case before the handler's own admin-role check ever runs.
	auto r = cli->Post("/api/writeback/all", httplib::Headers{}, "{}", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(WritebackAllTest, PostWritebackAll_RejectsViewer)
{
	auto r = cli->Post("/api/writeback/all", viewerHeaders(), "{}", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(WritebackAllTest, GetWritebackStatus_RejectsUnauthenticated)
{
	auto r = cli->Get("/api/writeback/status", httplib::Headers{});
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(WritebackAllTest, GetWritebackStatus_RejectsViewer)
{
	auto r = cli->Get("/api/writeback/status", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(WritebackAllTest, AdminTriggerRoundTripsToIdle)
{
	// Empty DB — zero eligible items, so the background pass finishes near-
	// instantly. This is asserting the route wiring and that the
	// writeback_running_ guard is correctly released on completion, not
	// anything about push behavior itself.
	auto post = cli->Post("/api/writeback/all", adminHeaders(), "{}", "application/json");
	ASSERT_TRUE(post);
	EXPECT_EQ(post->status, 202);
	EXPECT_EQ(json::parse(post->body).value("status", ""), "started");

	bool became_idle = false;
	for (int i = 0; i < 100; ++i)
	{
		auto status = cli->Get("/api/writeback/status", adminHeaders());
		ASSERT_TRUE(status);
		ASSERT_EQ(status->status, 200);
		if (!json::parse(status->body).value("running", true))
		{
			became_idle = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	EXPECT_TRUE(became_idle);
}

TEST_F(WritebackAllTest, AdminTriggerAcceptsLibraryAndSourceScopeBody)
{
	auto post = cli->Post("/api/writeback/all", adminHeaders(),
						  json{{"library_id", "lib-a"}, {"source_id", "src-a"}}.dump(),
						  "application/json");
	ASSERT_TRUE(post);
	EXPECT_EQ(post->status, 202);
}