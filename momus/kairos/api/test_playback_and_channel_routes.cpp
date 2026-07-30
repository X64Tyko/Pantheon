// Route-level coverage for the two Kairos endpoints Hephaestus calls
// directly, with no Authorization header at all (KairosClient::makeClient()
// attaches none): GET /api/channels (HDHomeRun lineup, M3U/XMLTV generation
// -- Hermes calls the same route the same way) and
// GET /api/playback/:content_type/:id (resolving a library item to a
// playable file before starting a transcode). Both are explicitly exempted
// from auth in Router.cpp's isPublicPath for exactly this reason. Neither
// had a dedicated test before this file.
//
// GET /api/channels is the interesting one: it also carries real
// parental-controls filtering, but only when currentUser() is populated
// (i.e. a real end-user token was sent, as Hades always does). An
// unauthenticated internal caller like Hephaestus/Hermes building a generic
// HDHomeRun lineup gets the *unfiltered* list by design -- there's no
// specific viewer to restrict against for that use case. That's a subtle
// enough distinction to be worth locking in with a test rather than leaving
// it to be "correct by accident."

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

class PlaybackAndChannelRoutesTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ConfStore conf{"./momus_playback_channel_test.conf"};
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

		auth.createUser("pc_viewer", "pc-password-1", "viewer");
		for (const auto& u : auth.listUsers()) if (u.username == "pc_viewer") viewer_id = u.user_id;
		viewer_token = auth.login("pc_viewer", "pc-password-1");
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
		std::error_code ec;
		std::filesystem::remove("./momus_playback_channel_test.conf", ec);
	}

	httplib::Headers viewerHeaders() const { return {{"Authorization", "Bearer " + viewer_token}}; }
};

// ---------------------------------------------------------------------------
// GET /api/channels
// ---------------------------------------------------------------------------

TEST_F(PlaybackAndChannelRoutesTest, ChannelsListWorksWithNoAuthAtAll)
{
	SQLite::Statement c(db.get(), "INSERT INTO channel (channel_id, name, number) VALUES ('c1','News',1)");
	c.exec();

	// No Authorization header -- exactly how Hephaestus/Hermes call this.
	auto r = cli->Get("/api/channels");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	ASSERT_EQ(body.size(), 1u);
	EXPECT_EQ(body[0]["channel_id"], "c1");
	EXPECT_EQ(body[0]["name"], "News");
}

// The subtle behavior this whole file exists to lock in: an unauthenticated
// caller sees every channel, restricted or not -- there's no specific
// viewer to filter against, so the internal-service (HDHomeRun lineup)
// use case must never silently drop rows.
TEST_F(PlaybackAndChannelRoutesTest, ChannelsListIsUnfilteredWithNoAuth)
{
	SQLite::Statement c1(db.get(), "INSERT INTO channel (channel_id, name, number, content_tag) VALUES ('c1','Kids',1,'TV-Y')");
	c1.exec();
	SQLite::Statement c2(db.get(), "INSERT INTO channel (channel_id, name, number, content_tag) VALUES ('c2','Adult',2,'TV-MA')");
	c2.exec();

	auto r = cli->Get("/api/channels");
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	EXPECT_EQ(body.size(), 2u);
}

TEST_F(PlaybackAndChannelRoutesTest, ChannelsListFiltersForRestrictedAuthenticatedViewer)
{
	SQLite::Statement c1(db.get(), "INSERT INTO channel (channel_id, name, number, content_tag) VALUES ('c1','Kids',1,'TV-Y')");
	c1.exec();
	SQLite::Statement c2(db.get(), "INSERT INTO channel (channel_id, name, number, content_tag) VALUES ('c2','Adult',2,'TV-MA')");
	c2.exec();
	auth.updateRestriction(viewer_id, true, "TV-Y", "G", "TV-Y");

	auto r = cli->Get("/api/channels", viewerHeaders());
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	ASSERT_EQ(body.size(), 1u);
	EXPECT_EQ(body[0]["channel_id"], "c1");
}

TEST_F(PlaybackAndChannelRoutesTest, ChannelsListShowsUnrestrictedAuthenticatedViewerEverything)
{
	SQLite::Statement c1(db.get(), "INSERT INTO channel (channel_id, name, number, content_tag) VALUES ('c1','Kids',1,'TV-Y')");
	c1.exec();
	SQLite::Statement c2(db.get(), "INSERT INTO channel (channel_id, name, number, content_tag) VALUES ('c2','Adult',2,'TV-MA')");
	c2.exec();

	auto r = cli->Get("/api/channels", viewerHeaders());
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	EXPECT_EQ(body.size(), 2u);
}

TEST_F(PlaybackAndChannelRoutesTest, ChannelsListOverrideRestoresBlockedChannelForRestrictedViewer)
{
	SQLite::Statement c1(db.get(), "INSERT INTO channel (channel_id, name, number, content_tag) VALUES ('c1','Adult',1,'TV-MA')");
	c1.exec();
	auth.updateRestriction(viewer_id, true, "TV-Y", "G", "TV-Y");
	RestrictionRepository(db).setOverride(viewer_id, "channel", "c1", "allow");

	auto r = cli->Get("/api/channels", viewerHeaders());
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	ASSERT_EQ(body.size(), 1u);
	EXPECT_EQ(body[0]["channel_id"], "c1");
}

// ---------------------------------------------------------------------------
// GET /api/playback/:content_type/:id
// ---------------------------------------------------------------------------

TEST_F(PlaybackAndChannelRoutesTest, PlaybackRejectsInvalidContentType)
{
	auto r = cli->Get("/api/playback/show/whatever");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 400);
}

TEST_F(PlaybackAndChannelRoutesTest, PlaybackWorksWithNoAuthAtAll)
{
	SQLite::Statement m(db.get(),
						"INSERT INTO movie(movie_id, title, file_path, duration_ms) VALUES ('m1','Movie One','/media/m1.mkv',5400000)");
	m.exec();

	// No Authorization header -- exactly how Hephaestus calls this.
	auto r = cli->Get("/api/playback/movie/m1");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	EXPECT_EQ(body["file_path"], "/media/m1.mkv");
	EXPECT_EQ(body["duration_ms"], 5400000);
	EXPECT_EQ(body["title"], "Movie One");
}

TEST_F(PlaybackAndChannelRoutesTest, PlaybackMovieNotFoundIs404)
{
	auto r = cli->Get("/api/playback/movie/does-not-exist");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 404);
}

TEST_F(PlaybackAndChannelRoutesTest, PlaybackMovieWithEmptyFilePathIs404)
{
	SQLite::Statement m(db.get(),
						"INSERT INTO movie(movie_id, title, file_path, duration_ms) VALUES ('m2','No File','',0)");
	m.exec();
	auto r = cli->Get("/api/playback/movie/m2");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 404);
}

TEST_F(PlaybackAndChannelRoutesTest, PlaybackEpisodeResolvesToItsOwnFileWhenNotLinked)
{
	SQLite::Statement s(db.get(), "INSERT INTO show(show_id, title) VALUES ('s1','Show One')");
	s.exec();
	SQLite::Statement e(db.get(),
						"INSERT INTO episode (episode_id, show_id, title, thumb, season, episode, duration_ms, file_path) "
						"VALUES ('e1', 's1', 'Ep 1', '', 1, 1, 1200000, '/media/e1.mkv')");
	e.exec();

	auto r = cli->Get("/api/playback/episode/e1");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	EXPECT_EQ(body["file_path"], "/media/e1.mkv");
	EXPECT_EQ(body["duration_ms"], 1200000);
	EXPECT_EQ(body["title"], "Ep 1");
}

// The show-specials-linking feature: an episode with linked_movie_id set
// (an OVA/special that actually lives in the Movies library) must resolve
// to the LINKED MOVIE's file_path/duration_ms, not its own -- this is the
// one piece of real branching logic in this route, and it had no coverage
// at all before this test.
TEST_F(PlaybackAndChannelRoutesTest, PlaybackLinkedSpecialEpisodeResolvesToLinkedMovieFile)
{
	SQLite::Statement s(db.get(), "INSERT INTO show(show_id, title) VALUES ('s1','Show One')");
	s.exec();
	SQLite::Statement m(db.get(),
						"INSERT INTO movie(movie_id, title, file_path, duration_ms) VALUES ('m1','The Special Movie','/media/special.mkv',3600000)");
	m.exec();
	SQLite::Statement e(db.get(),
						"INSERT INTO episode (episode_id, show_id, title, thumb, season, episode, duration_ms, file_path, linked_movie_id) "
						"VALUES ('e-special', 's1', 'OVA', '', 0, 1, 0, '', 'm1')");
	e.exec();

	auto r = cli->Get("/api/playback/episode/e-special");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	EXPECT_EQ(body["file_path"], "/media/special.mkv");
	EXPECT_EQ(body["duration_ms"], 3600000);
	// Title always comes from the episode row itself, even when the file
	// resolves through the linked movie.
	EXPECT_EQ(body["title"], "OVA");
}

// ---------------------------------------------------------------------------
// POST /api/channels/:id/played
// ---------------------------------------------------------------------------
// Regression coverage for a real vulnerability (see CHANGELOG): this route
// used to be covered by isPublicPath's `path.ends_with("/played")` rule with
// no method restriction at all, unlike the sibling internal-service rules
// right above it in Router.cpp -- meaning it was reachable, unauthenticated,
// by any LAN client (Kairos's port is published directly to the host by
// default). Unlike GET /api/channels above, this route *mutates* live
// scheduling state, so it needs its own machine credential rather than the
// "no session, no filtering to apply" reasoning that makes an open GET safe.

TEST_F(PlaybackAndChannelRoutesTest, PlayedRejectsRequestWithNoInternalToken)
{
	SQLite::Statement c(db.get(), "INSERT INTO channel (channel_id, name, number) VALUES ('c1','News',1)");
	c.exec();

	auto r = cli->Post("/api/channels/c1/played", R"({"item_id":"x"})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(PlaybackAndChannelRoutesTest, PlayedRejectsRequestWithWrongInternalToken)
{
	SQLite::Statement c(db.get(), "INSERT INTO channel (channel_id, name, number) VALUES ('c1','News',1)");
	c.exec();

	httplib::Headers headers{{"X-Internal-Token", "not-the-real-token"}};
	auto r = cli->Post("/api/channels/c1/played", headers, R"({"item_id":"x"})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(PlaybackAndChannelRoutesTest, PlayedAcceptsRequestWithCorrectInternalToken)
{
	SQLite::Statement c(db.get(), "INSERT INTO channel (channel_id, name, number) VALUES ('c1','News',1)");
	c.exec();

	httplib::Headers headers{{"X-Internal-Token", conf.getInternalToken()}};
	auto r = cli->Post("/api/channels/c1/played", headers, R"({"item_id":"x"})", "application/json");
	ASSERT_TRUE(r);
	// Auth passed (not 401); whatever markPlayed does with a bare item_id and
	// no matching schedule row is that handler's own business logic, not
	// this test's concern.
	EXPECT_NE(r->status, 401);
}