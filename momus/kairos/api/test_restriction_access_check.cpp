// Route-level coverage for the two "access-check" endpoints
// (GET /api/content/:type/:id/access-check, GET /api/channels/:id/access-check)
// and GET /api/auth/me is covered alongside AuthService's other routes in
// test_auth_service_routes.cpp.
//
// These two endpoints are the actual parental-controls enforcement boundary
// for playback: Hermes's authedHephaestusProxy (hermes/src/api/Router.cpp)
// calls back into them, plus /api/auth/me, before letting a VOD/preview
// stream start -- Hermes itself has no restriction logic of its own, it
// trusts whatever these return. Before this file existed, neither endpoint
// had any dedicated test on either side of that boundary. Same
// real-Router-plus-real-HTTP-server pattern as
// test_content_service_security.cpp / test_auth_service_routes.cpp.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

#include "api/Router.h"
#include "auth/AuthStore.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include "db/RestrictionRepository.h"
#include "download/DownloadManager.h"
#include "email/EmailService.h"
#include "log/LogBuffer.h"
#include "backup/BackupManager.h"
#include "jobs/JobScheduler.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "source/SyncManager.h"

using json = nlohmann::json;

class RestrictionAccessCheckTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ConfStore conf{"./momus_access_check_test.conf"};
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

	std::string viewer_id;
	std::string viewer_token;

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

		auth.createUser("access_viewer", "access-password-1", "viewer");
		for (const auto& u : auth.listUsers()) if (u.username == "access_viewer") viewer_id = u.user_id;
		viewer_token = auth.login("access_viewer", "access-password-1");

		SQLite::Statement m1(db.get(),
							 "INSERT INTO movie(movie_id, title, file_path, duration_ms, content_rating) VALUES (?,?,?,?,?)");
		m1.bind(1, "movie-r");
		m1.bind(2, "Restricted Movie");
		m1.bind(3, "/media/movie-r.mkv");
		m1.bind(4, 5400000);
		m1.bind(5, "R");
		m1.exec();

		SQLite::Statement m2(db.get(),
							 "INSERT INTO movie(movie_id, title, file_path, duration_ms, content_rating) VALUES (?,?,?,?,?)");
		m2.bind(1, "movie-g");
		m2.bind(2, "Kid-Safe Movie");
		m2.bind(3, "/media/movie-g.mkv");
		m2.bind(4, 5400000);
		m2.bind(5, "G");
		m2.exec();

		SQLite::Statement s1(db.get(),
							 "INSERT INTO show(show_id, title, content_rating) VALUES (?,?,?)");
		s1.bind(1, "show-ma");
		s1.bind(2, "Restricted Show");
		s1.bind(3, "TV-MA");
		s1.exec();

		SQLite::Statement e1(db.get(),
							 "INSERT INTO episode (episode_id, show_id, title, thumb, season, episode, duration_ms, file_path) "
							 "VALUES ('ep-1', 'show-ma', 'Ep 1', '', 1, 1, 0, '/media/ep1.mkv')");
		e1.exec();

		SQLite::Statement c1(db.get(),
							 "INSERT INTO channel (channel_id, name, number, content_tag) VALUES (?,?,?,?)");
		c1.bind(1, "chan-ma");
		c1.bind(2, "Restricted Channel");
		c1.bind(3, 1);
		c1.bind(4, "TV-MA");
		c1.exec();
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
		std::error_code ec;
		std::filesystem::remove("./momus_access_check_test.conf", ec);
	}

	httplib::Headers viewerHeaders() const { return {{"Authorization", "Bearer " + viewer_token}}; }

	void restrictViewer(const std::string& max_movie, const std::string& max_tv, const std::string& max_channel)
	{
		auth.updateRestriction(viewer_id, true, max_tv, max_movie, max_channel);
	}
};

// ---------------------------------------------------------------------------
// /api/content/:type/:id/access-check
// ---------------------------------------------------------------------------

TEST_F(RestrictionAccessCheckTest, ContentAccessCheckRequiresAuth)
{
	auto r = cli->Get("/api/content/movie/movie-r/access-check");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(RestrictionAccessCheckTest, ContentAccessCheckAllowsUnrestrictedUser)
{
	auto r = cli->Get("/api/content/movie/movie-r/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_TRUE(json::parse(r->body)["allowed"]);
}

TEST_F(RestrictionAccessCheckTest, ContentAccessCheckBlocksRestrictedMovieOverCeiling)
{
	restrictViewer(/*max_movie=*/"PG", /*max_tv=*/"TV-PG", /*max_channel=*/"TV-PG");
	auto r = cli->Get("/api/content/movie/movie-r/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_FALSE(json::parse(r->body)["allowed"]);
}

TEST_F(RestrictionAccessCheckTest, ContentAccessCheckAllowsRestrictedMovieWithinCeiling)
{
	restrictViewer("PG-13", "TV-PG", "TV-PG");
	auto r = cli->Get("/api/content/movie/movie-g/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_TRUE(json::parse(r->body)["allowed"]);
}

TEST_F(RestrictionAccessCheckTest, ContentAccessCheckShowUsesTvScale)
{
	restrictViewer("PG", "TV-14", "TV-14");
	auto r = cli->Get("/api/content/show/show-ma/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_FALSE(json::parse(r->body)["allowed"]);
}

TEST_F(RestrictionAccessCheckTest, ContentAccessCheckEpisodeResolvesToParentShowRating)
{
	restrictViewer("PG", "TV-14", "TV-14");
	auto r = cli->Get("/api/content/episode/ep-1/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	// ep-1 belongs to show-ma (TV-MA), same as the direct show check above.
	EXPECT_FALSE(json::parse(r->body)["allowed"]);
}

// RatingSeverity treats an unrecognized/blank rating as the top ("Adult")
// tier -- fail closed. A restricted user with any realistic ceiling must
// never be waved through just because the target content_id doesn't exist
// (or hasn't been rated yet).
TEST_F(RestrictionAccessCheckTest, ContentAccessCheckUnknownMovieFailsClosedForRestrictedUser)
{
	restrictViewer("NC-17", "TV-MA", "TV-MA");
	auto r = cli->Get("/api/content/movie/does-not-exist/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_FALSE(json::parse(r->body)["allowed"]);
}

TEST_F(RestrictionAccessCheckTest, ContentAccessCheckOverrideWinsOverCeiling)
{
	restrictViewer("PG", "TV-PG", "TV-PG");
	RestrictionRepository(db).setOverride(viewer_id, "movie", "movie-r", "allow");
	auto r = cli->Get("/api/content/movie/movie-r/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_TRUE(json::parse(r->body)["allowed"]);
}

// ---------------------------------------------------------------------------
// /api/channels/:id/access-check
// ---------------------------------------------------------------------------

TEST_F(RestrictionAccessCheckTest, ChannelAccessCheckRequiresAuth)
{
	auto r = cli->Get("/api/channels/chan-ma/access-check");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(RestrictionAccessCheckTest, ChannelAccessCheckUnknownChannelReturns404)
{
	auto r = cli->Get("/api/channels/does-not-exist/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 404);
}

TEST_F(RestrictionAccessCheckTest, ChannelAccessCheckAllowsUnrestrictedUser)
{
	auto r = cli->Get("/api/channels/chan-ma/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_TRUE(json::parse(r->body)["allowed"]);
}

TEST_F(RestrictionAccessCheckTest, ChannelAccessCheckBlocksRestrictedUserOverCeiling)
{
	restrictViewer("PG", "TV-PG", "TV-PG");
	auto r = cli->Get("/api/channels/chan-ma/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_FALSE(json::parse(r->body)["allowed"]);
}

TEST_F(RestrictionAccessCheckTest, ChannelAccessCheckOverrideWinsOverCeiling)
{
	restrictViewer("PG", "TV-PG", "TV-PG");
	RestrictionRepository(db).setOverride(viewer_id, "channel", "chan-ma", "allow");
	auto r = cli->Get("/api/channels/chan-ma/access-check", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_TRUE(json::parse(r->body)["allowed"]);
}