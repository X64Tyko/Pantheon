// Security regression tests for GET/PATCH /api/scrapers/config — this
// endpoint used to return the real TMDB/TVDB/Trakt/AniDB api_key (and TVDB's
// subscriber pin) in plaintext on every GET, breaking the masking convention
// every sibling credentials endpoint follows (GET /api/config/sources,
// GET /api/config/credentials/:source_id — has_token booleans only). Same
// real-Router-plus-real-HTTP-requests pattern as
// test_content_service_security.cpp.

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
} // namespace

class ScraperServiceSecurityTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	std::string conf_path = (fs::temp_directory_path() / ("momus_scrapertest_conf_" + randomSuffix())).string();
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

		auth.createUser("scrapertest_admin", "scrapertest-password", "admin");
		admin_token = auth.login("scrapertest_admin", "scrapertest-password");
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
		std::error_code ec;
		fs::remove(conf_path, ec);
	}

	httplib::Headers adminHeaders() const { return {{"Authorization", "Bearer " + admin_token}}; }

	json tmdbConfig(const json& configs_array)
	{
		for (const auto& c : configs_array) if (c["source"] == "tmdb") return c;
		return json();
	}

	json tvdbConfig(const json& configs_array)
	{
		for (const auto& c : configs_array) if (c["source"] == "tvdb") return c;
		return json();
	}
};

TEST_F(ScraperServiceSecurityTest, GetNeverReturnsRealApiKeyOrPin)
{
	const std::string real_key = "SUPER-SECRET-TMDB-KEY-12345";
	json patch                 = {{"configs", json::array({{{"source", "tmdb"}, {"api_key", real_key}}})}};
	auto rp                    = cli->Patch("/api/scrapers/config", adminHeaders(), patch.dump(), "application/json");
	ASSERT_TRUE(rp);
	ASSERT_EQ(rp->status, 200);

	auto rg = cli->Get("/api/scrapers/config", adminHeaders());
	ASSERT_TRUE(rg);
	ASSERT_EQ(rg->status, 200);
	// Not just "the field doesn't equal the key" — the real key must not
	// appear ANYWHERE in the response body at all.
	EXPECT_EQ(rg->body.find(real_key), std::string::npos);

	json tmdb = tmdbConfig(json::parse(rg->body)["configs"]);
	ASSERT_FALSE(tmdb.is_null());
	EXPECT_EQ(tmdb["api_key"], "");
	EXPECT_TRUE(tmdb["has_api_key"].get<bool>());
}

TEST_F(ScraperServiceSecurityTest, GetReportsHasApiKeyFalseWhenNoneConfigured)
{
	auto rg = cli->Get("/api/scrapers/config", adminHeaders());
	ASSERT_TRUE(rg);
	ASSERT_EQ(rg->status, 200);
	json tmdb = tmdbConfig(json::parse(rg->body)["configs"]);
	ASSERT_FALSE(tmdb.is_null());
	EXPECT_EQ(tmdb["api_key"], "");
	EXPECT_FALSE(tmdb["has_api_key"].get<bool>());
}

// The Hades Settings page resends the WHOLE scraper form on every save, not
// just changed fields (see SettingsPage.tsx's updateScraperConfig) — since
// GET never returns the real key anymore, an untouched field round-trips as
// "" on save. PATCH must treat that as "leave unchanged," not "clear the
// key," or every unrelated settings change (e.g. toggling "enabled") would
// silently wipe the stored key.
TEST_F(ScraperServiceSecurityTest, PatchWithEmptyApiKeyDoesNotWipeAnExistingKey)
{
	const std::string real_key = "SUPER-SECRET-TMDB-KEY-67890";
	json set_key               = {{"configs", json::array({{{"source", "tmdb"}, {"api_key", real_key}}})}};
	ASSERT_TRUE(cli->Patch("/api/scrapers/config", adminHeaders(), set_key.dump(), "application/json"));

	// Simulates resaving the form with the field left blank (as it now
	// always is after a GET) while toggling an unrelated field.
	json resave = {{"configs", json::array({{{"source", "tmdb"}, {"api_key", ""}, {"enabled", false}}})}};
	auto rp     = cli->Patch("/api/scrapers/config", adminHeaders(), resave.dump(), "application/json");
	ASSERT_TRUE(rp);
	ASSERT_EQ(rp->status, 200);

	auto rg = cli->Get("/api/scrapers/config", adminHeaders());
	ASSERT_TRUE(rg);
	json tmdb = tmdbConfig(json::parse(rg->body)["configs"]);
	ASSERT_FALSE(tmdb.is_null());
	EXPECT_TRUE(tmdb["has_api_key"].get<bool>()) << "key was wiped by a resave that left api_key blank";
	EXPECT_FALSE(tmdb["enabled"].get<bool>()) << "the actually-intended field change didn't take effect";
}

TEST_F(ScraperServiceSecurityTest, GetNeverReturnsRealTvdbPin)
{
	const std::string real_pin = "SECRET-TVDB-PIN-999";
	json patch                 = {{"configs", json::array({{{"source", "tvdb"}, {"pin", real_pin}}})}};
	ASSERT_TRUE(cli->Patch("/api/scrapers/config", adminHeaders(), patch.dump(), "application/json"));

	auto rg = cli->Get("/api/scrapers/config", adminHeaders());
	ASSERT_TRUE(rg);
	EXPECT_EQ(rg->body.find(real_pin), std::string::npos);

	json tvdb = tvdbConfig(json::parse(rg->body)["configs"]);
	ASSERT_FALSE(tvdb.is_null());
	EXPECT_EQ(tvdb["pin"], "");
	EXPECT_TRUE(tvdb["has_pin"].get<bool>());
}

TEST_F(ScraperServiceSecurityTest, ScraperConfigStillAdminGated)
{
	// No token at all — rejected by the pre-routing auth gate before ever
	// reaching ScraperService's own admin-role check (see Router.cpp).
	auto r_noauth = cli->Get("/api/scrapers/config");
	ASSERT_TRUE(r_noauth);
	EXPECT_EQ(r_noauth->status, 401);

	// A valid session that isn't an admin — rejected by ScraperService's own
	// role check instead.
	auth.createUser("scrapertest_viewer", "scrapertest-password-2", "viewer");
	const std::string viewer_token = auth.login("scrapertest_viewer", "scrapertest-password-2");
	auto r_viewer                  = cli->Get("/api/scrapers/config", {{"Authorization", "Bearer " + viewer_token}});
	ASSERT_TRUE(r_viewer);
	EXPECT_EQ(r_viewer->status, 403);
}