// Regression coverage for POST /api/config/library/reset — reported live as
// "Can't delete DB because of FK watch progress. [error] POST
// /api/config/library/reset: FOREIGN KEY constraint failed". SQLite's FK
// violation message never names the actual table, and watch_progress turns
// out to have no FK to episode/movie/show at all (verified via
// pragma_foreign_key_list against a real dev DB) — the real culprits are
// show_track_preference/movie_track_preference (migrations v92/v94), which
// reference show(show_id)/movie(movie_id) with no ON DELETE CASCADE and
// predate this route ever being updated to know about them. Same root cause
// class as AuthStore::deleteUserCascade's fix for user deletion, just a
// second, independent spot that needed the same two tables added.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
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

using json = nlohmann::json;

class LibraryResetRouteTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ConfStore conf{"./momus_library_reset_test.conf"};
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

	std::string admin_token, viewer_token, viewer_id;

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

		auth.createUser("reset_admin", "reset-password-1", "admin");
		auth.createUser("reset_viewer", "reset-password-2", "viewer");
		for (const auto& u : auth.listUsers()) if (u.username == "reset_viewer") viewer_id = u.user_id;
		admin_token  = auth.login("reset_admin", "reset-password-1");
		viewer_token = auth.login("reset_viewer", "reset-password-2");
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
	}

	httplib::Headers adminHeaders() const { return {{"Authorization", "Bearer " + admin_token}}; }

	void seedShowAndMovie()
	{
		db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1', 'Show One')");
		db.get().exec("INSERT INTO movie (movie_id, title, file_path, duration_ms) VALUES ('m1', 'Movie One', '/m.mkv', 7200000)");
	}

	int64_t rowCount(const char* table)
	{
		SQLite::Statement q(db.get(), std::string("SELECT COUNT(*) FROM ") + table);
		q.executeStep();
		return q.getColumn(0).getInt64();
	}
};

TEST_F(LibraryResetRouteTest, RequiresAdmin)
{
	auto rNoAuth = cli->Post("/api/config/library/reset");
	ASSERT_TRUE(rNoAuth);
	EXPECT_EQ(rNoAuth->status, 401);

	auto rViewer = cli->Post("/api/config/library/reset", {{"Authorization", "Bearer " + viewer_token}}, "", "");
	ASSERT_TRUE(rViewer);
	EXPECT_EQ(rViewer->status, 403);
}

TEST_F(LibraryResetRouteTest, SucceedsOnAnEmptyLibrary)
{
	auto r = cli->Post("/api/config/library/reset", adminHeaders(), "", "");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
}

TEST_F(LibraryResetRouteTest, WipesShowsEpisodesAndMovies)
{
	seedShowAndMovie();
	ASSERT_EQ(rowCount("show"), 1);
	ASSERT_EQ(rowCount("movie"), 1);

	auto r = cli->Post("/api/config/library/reset", adminHeaders(), "", "");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_EQ(rowCount("show"), 0);
	EXPECT_EQ(rowCount("movie"), 0);
}

// The actual regression: previously threw "FOREIGN KEY constraint failed"
// (500) instead of resetting anything, whenever a show/movie had a per-user
// track preference row against it.
TEST_F(LibraryResetRouteTest, SucceedsWhenAShowHasATrackPreference)
{
	seedShowAndMovie();
	SQLite::Statement sp(db.get(),
						 "INSERT INTO show_track_preference (user_id, show_id, updated_at) VALUES (?, 's1', 1000)");
	sp.bind(1, viewer_id);
	sp.exec();

	auto r = cli->Post("/api/config/library/reset", adminHeaders(), "", "");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200) << r->body;
	EXPECT_EQ(rowCount("show"), 0);
	EXPECT_EQ(rowCount("show_track_preference"), 0);
}

TEST_F(LibraryResetRouteTest, SucceedsWhenAMovieHasATrackPreference)
{
	seedShowAndMovie();
	SQLite::Statement mp(db.get(),
						 "INSERT INTO movie_track_preference (user_id, movie_id, updated_at) VALUES (?, 'm1', 1000)");
	mp.bind(1, viewer_id);
	mp.exec();

	auto r = cli->Post("/api/config/library/reset", adminHeaders(), "", "");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200) << r->body;
	EXPECT_EQ(rowCount("movie"), 0);
	EXPECT_EQ(rowCount("movie_track_preference"), 0);
}

TEST_F(LibraryResetRouteTest, UsersAndSourcesSurviveAReset)
{
	seedShowAndMovie();
	auto r = cli->Post("/api/config/library/reset", adminHeaders(), "", "");
	ASSERT_TRUE(r);
	ASSERT_EQ(r->status, 200);
	EXPECT_EQ(auth.listUsers().size(), 2u);
}