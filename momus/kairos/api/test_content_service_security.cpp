// Security regression tests for the "local:" poster-caching sentinel added
// alongside ScraperManager::persistAnidbThumbLocally (see anidb_download_posters
// in ScraperSettings). These spin up a REAL Router — identical wiring to
// main.cpp — and fire REAL HTTP requests containing actual attack payloads
// (e.g. "local:/etc/passwd") at the actual running endpoints. Each test's job
// is to prove the attack FAILS: the server must reject the request and the
// target file's contents must never appear in any response. A test that
// would leak a real secret if the guard regressed is exactly the point — a
// meaningful regression check, not a reimplementation of the logic under test.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include "api/Router.h"
#include "api/RouteHelpers.h"
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

	// A file the test process can read but no attack should ever be able to
	// exfiltrate through the API. Content is a canary string unique to this run,
	// so a match can only mean the vulnerability regressed.
	struct CanaryFile
	{
		fs::path path;
		std::string secret;

		CanaryFile()
		{
			secret = "KAIROS_SECURITY_CANARY_" + randomSuffix();
			path   = fs::temp_directory_path() / ("momus_sectest_canary_" + randomSuffix() + ".txt");
			std::ofstream f(path);
			f << secret;
		}

		~CanaryFile()
		{
			std::error_code ec;
			fs::remove(path, ec);
		}
	};

	// Same shape as what ScraperManager::persistAnidbThumbLocally itself writes
	// (aniThumb.jpg with real bytes) — used for the positive-control test.
	struct LegitPosterFile
	{
		fs::path path;
		std::string content = "NOT-A-SECRET-JUST-A-TEST-POSTER-BYTES";

		LegitPosterFile()
		{
			path = fs::temp_directory_path() / ("momus_sectest_aniThumb_" + randomSuffix() + ".jpg");
			std::ofstream f(path, std::ios::binary);
			f << content;
		}

		~LegitPosterFile()
		{
			std::error_code ec;
			fs::remove(path, ec);
		}
	};

	std::string bodyOf(const httplib::Result& r) { return r ? r->body : std::string(); }
} // namespace

class ContentServiceSecurityTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	std::string conf_path = (fs::temp_directory_path() / ("momus_sectest_conf_" + randomSuffix())).string();
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
	const std::string show_id  = "sectest-show-1";
	const std::string movie_id = "sectest-movie-1";

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

		auth.createUser("sectest_admin", "sectest-password-1", "admin");
		auth.createUser("sectest_viewer", "sectest-password-2", "viewer");
		admin_token  = auth.login("sectest_admin", "sectest-password-1");
		viewer_token = auth.login("sectest_viewer", "sectest-password-2");

		SQLite::Statement s(db.get(), "INSERT INTO show(show_id, title) VALUES (?, ?)");
		s.bind(1, show_id);
		s.bind(2, "Security Test Show");
		s.exec();
		SQLite::Statement m(db.get(),
							"INSERT INTO movie(movie_id, title, file_path, duration_ms) VALUES (?, ?, ?, ?)");
		m.bind(1, movie_id);
		m.bind(2, "Security Test Movie");
		m.bind(3, "/media/movies/Security Test Movie/movie.mkv");
		m.bind(4, 5400000);
		m.exec();
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

TEST_F(ContentServiceSecurityTest, ImagesProxyRejectsUnauthenticatedLocalRead)
{
	CanaryFile canary;

	auto r = cli->Get("/api/images/proxy?url=local:" + route::urlEncode(canary.path.string()));
	EXPECT_TRUE(r);
	EXPECT_EQ(r->status, 400);
	EXPECT_EQ(bodyOf(r).find(canary.secret), std::string::npos);

	auto r2 = cli->Get("/api/images/proxy?url=local:/etc/passwd");
	EXPECT_TRUE(r2);
	EXPECT_EQ(r2->status, 400);
	EXPECT_EQ(bodyOf(r2).find("root:"), std::string::npos);
}

// SSRF guard on GET /api/images/proxy — this endpoint is unauthenticated
// (see isPublicPath in Router.cpp) and fetches whatever `url` it's given
// server-side, so without the private/loopback/link-local check in
// ContentService.cpp's isSafeExternalHost it's an open proxy an outside
// attacker can point at internal Docker services or a cloud metadata
// endpoint. Every case here must come back 400 WITHOUT the server ever
// attempting the actual fetch (that's what makes these fast/deterministic
// instead of depending on real network access or a listener on these ports).
TEST_F(ContentServiceSecurityTest, ImagesProxyRejectsPrivateAndLoopbackIPs)
{
	for (const std::string& target : {
			 "http://127.0.0.1/secret",
			 "http://127.0.0.1:8080/api/config/sources", // Kairos's own admin API, same host
			 "http://localhost/secret",
			 "http://169.254.169.254/latest/meta-data/iam/security-credentials/", // cloud metadata
			 "http://10.0.0.5/secret",
			 "http://172.17.0.1/secret", // default Docker bridge gateway
			 "http://192.168.1.1/secret",
			 "http://[::1]/secret",
		 })
	{
		auto r = cli->Get("/api/images/proxy?url=" + route::urlEncode(target));
		ASSERT_TRUE(r) << "request itself failed for " << target;
		EXPECT_EQ(r->status, 400) << "expected SSRF guard to reject " << target;
	}
}

TEST_F(ContentServiceSecurityTest, ImagesProxyRejectsNonHttpSchemes)
{
	for (const std::string& target : {"file:///etc/passwd", "ftp://example.com/x", "gopher://example.com/x"})
	{
		auto r = cli->Get("/api/images/proxy?url=" + route::urlEncode(target));
		ASSERT_TRUE(r) << "request itself failed for " << target;
		EXPECT_EQ(r->status, 400) << "expected scheme guard to reject " << target;
	}
}

TEST_F(ContentServiceSecurityTest, PatchShowRejectsForgedLocalSentinel)
{
	CanaryFile canary;

	json thumb_body = {{"thumb", "local:" + canary.path.string()}};
	auto r          = cli->Patch("/api/shows/" + show_id, adminHeaders(), thumb_body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 400);
	EXPECT_NE(r->body.find("invalid thumb value"), std::string::npos);

	json art_body = {{"art", "local:" + canary.path.string()}};
	auto r2       = cli->Patch("/api/shows/" + show_id, adminHeaders(), art_body.dump(), "application/json");
	ASSERT_TRUE(r2);
	EXPECT_EQ(r2->status, 400);

	// Path-traversal-flavored variant — must still be blocked outright
	// regardless of what follows the "local:" prefix.
	json traversal_body = {{"thumb", "local:../../../../../../../../etc/passwd"}};
	auto r3             = cli->Patch("/api/shows/" + show_id, adminHeaders(), traversal_body.dump(), "application/json");
	ASSERT_TRUE(r3);
	EXPECT_EQ(r3->status, 400);

	// Case-variant bypass attempt against the sentinel check itself.
	json case_body = {{"thumb", "LOCAL:" + canary.path.string()}};
	auto r4        = cli->Patch("/api/shows/" + show_id, adminHeaders(), case_body.dump(), "application/json");
	ASSERT_TRUE(r4);
	EXPECT_EQ(r4->status, 400);

	// None of the rejected PATCHes should have taken effect.
	auto rt = cli->Get(("/api/shows/" + show_id + "/thumb").c_str(), viewerHeaders());
	ASSERT_TRUE(rt);
	EXPECT_EQ(rt->status, 404);
	EXPECT_EQ(rt->body.find(canary.secret), std::string::npos);
}

TEST_F(ContentServiceSecurityTest, PatchMovieRejectsForgedLocalSentinel)
{
	CanaryFile canary;

	json thumb_body = {{"thumb", "local:" + canary.path.string()}};
	auto r          = cli->Patch("/api/movies/" + movie_id, adminHeaders(), thumb_body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 400);

	json art_body = {{"art", "local:" + canary.path.string()}};
	auto r2       = cli->Patch("/api/movies/" + movie_id, adminHeaders(), art_body.dump(), "application/json");
	ASSERT_TRUE(r2);
	EXPECT_EQ(r2->status, 400);

	auto rt = cli->Get(("/api/movies/" + movie_id + "/thumb").c_str(), viewerHeaders());
	ASSERT_TRUE(rt);
	EXPECT_EQ(rt->status, 404);
	EXPECT_EQ(rt->body.find(canary.secret), std::string::npos);
}

TEST_F(ContentServiceSecurityTest, AuthBoundariesStayIntact)
{
	auto r_noauth = cli->Get(("/api/shows/" + show_id + "/thumb").c_str());
	ASSERT_TRUE(r_noauth);
	EXPECT_EQ(r_noauth->status, 401);

	json body     = {{"thumb", "https://example.com/harmless.jpg"}};
	auto r_viewer = cli->Patch("/api/shows/" + show_id, viewerHeaders(), body.dump(), "application/json");
	ASSERT_TRUE(r_viewer);
	EXPECT_EQ(r_viewer->status, 403);

	auto r_noauth_patch = cli->Patch("/api/shows/" + show_id, body.dump(), "application/json");
	ASSERT_TRUE(r_noauth_patch);
	EXPECT_EQ(r_noauth_patch->status, 401);
}

TEST_F(ContentServiceSecurityTest, LegitimateLocalPosterStillServes)
{
	// Only ScraperManager::persistAnidbThumbLocally is meant to write this
	// shape of value — simulate exactly that (a direct DB write, not the
	// admin-editable HTTP field) to prove the fix didn't break the feature
	// it was added for.
	LegitPosterFile poster;
	SQLite::Statement upd(db.get(), "UPDATE show SET thumb = ? WHERE show_id = ?");
	upd.bind(1, "local:" + poster.path.string());
	upd.bind(2, show_id);
	upd.exec();

	auto r = cli->Get(("/api/shows/" + show_id + "/thumb").c_str(), viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_EQ(r->body, poster.content);
}

// Baseline hardening headers (see Router.cpp's set_post_routing_handler) —
// checked against an arbitrary real endpoint since they're applied to every
// response, not endpoint-specific. A regression here (e.g. the handler being
// replaced rather than extended, which is exactly the httplib footgun this
// guards against — only one post-routing handler can be registered at a
// time) would silently drop these from the entire API.
TEST_F(ContentServiceSecurityTest, BaselineSecurityHeadersPresentOnEveryResponse)
{
	auto r = cli->Get("/api/auth/me", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->get_header_value("X-Content-Type-Options"), "nosniff");
	EXPECT_EQ(r->get_header_value("X-Frame-Options"), "DENY");
	EXPECT_EQ(r->get_header_value("Referrer-Policy"), "same-origin");
	EXPECT_NE(r->get_header_value("Strict-Transport-Security").find("max-age="), std::string::npos);
}

// GET /api/logs/stream previously had no auth at all and held an
// API-thread-pool slot open indefinitely (see isPublicPath's own comment in
// Router.cpp) — a handful of concurrent anonymous connections could exhaust
// the whole pool and freeze every other request. Now admin-only. This test
// only checks the rejection — actually opening the stream and reading SSE
// data is exercised manually/in the live app, not worth the complexity of a
// chunked-response test here.
TEST_F(ContentServiceSecurityTest, LogsStreamRejectsUnauthenticatedAndNonAdminCallers)
{
	auto r_noauth = cli->Get("/api/logs/stream");
	ASSERT_TRUE(r_noauth);
	EXPECT_EQ(r_noauth->status, 401);

	auto r_viewer = cli->Get("/api/logs/stream", viewerHeaders());
	ASSERT_TRUE(r_viewer);
	EXPECT_EQ(r_viewer->status, 403);
}