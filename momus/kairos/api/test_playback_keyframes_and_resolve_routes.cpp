// Regression coverage for four PlaybackService.cpp routes shipped with no
// test coverage at all:
//
//   * GET /api/playback/:content_type/:id's new keyframes_ms/keyframes_size/
//     keyframes_mtime fields (the direct-stream keyframe cache — see
//     Database.cpp's v98 migration and SyncManager::syncMediaProbeFromFiles).
//   * PUT /api/playback/:content_type/:id/keyframes — Hephaestus's push-back
//     of its own fallback ffprobe scan, gated on the shared internal-token
//     mechanism (same bucket as POST .../played — see
//     test_playback_and_channel_routes.cpp for the pattern this mirrors).
//   * GET /api/shows/:id/watch-state, GET /api/movies/:id/resolve-play-target,
//     GET /api/shows/:id/resolve-play-target — server-side ports of Hades'
//     resolvePlayTarget.ts, all real-auth-gated (unlike the routes above).
//
// Same fixture shape as test_playback_and_channel_routes.cpp: a real Router
// wired to an in-memory Database, a real httplib::Server on a test port, and
// plain HTTP calls via httplib::Client.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
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

class PlaybackKeyframesAndResolveRoutesTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ConfStore conf{"./momus_playback_keyframes_resolve_test.conf"};
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

	// GET /api/playback/:content_type/:id now 404s unless the resolved file
	// actually exists on disk (see PlaybackService.cpp) — media_dir is a real
	// temp directory /media gets path-mapped to; insertMovie/insertEpisode's
	// default file_path values (x.mkv/e.mkv) plus special.mkv (used by the
	// linked-special test) are touched into it below so the routes this
	// fixture exercises keep resolving.
	std::filesystem::path media_dir;

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

		auth.createUser("kf_viewer", "kf-password-1", "viewer");
		for (const auto& u : auth.listUsers()) if (u.username == "kf_viewer") viewer_id = u.user_id;
		viewer_token = auth.login("kf_viewer", "kf-password-1");

		media_dir = std::filesystem::temp_directory_path() / "momus_playback_keyframes_resolve_test_media";
		std::filesystem::create_directories(media_dir);
		conf.setPathMaps("_test_media", {{"/media", media_dir.string()}});
		for (const char* name : {"x.mkv", "e.mkv", "special.mkv"}) std::ofstream(media_dir / name).put('x');
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
		std::error_code ec;
		std::filesystem::remove("./momus_playback_keyframes_resolve_test.conf", ec);
		std::filesystem::remove_all(media_dir, ec);
	}

	httplib::Headers viewerHeaders() const { return {{"Authorization", "Bearer " + viewer_token}}; }
	httplib::Headers internalHeaders() const { return {{"X-Internal-Token", conf.getInternalToken()}}; }

	void insertMovie(const std::string& id, const std::string& title, const std::string& file_path = "/media/x.mkv",
					 int64_t duration_ms                                                           = 1000)
	{
		SQLite::Statement s(db.get(),
							"INSERT INTO movie(movie_id, title, file_path, duration_ms) VALUES (?,?,?,?)");
		s.bind(1, id);
		s.bind(2, title);
		s.bind(3, file_path);
		s.bind(4, duration_ms);
		s.exec();
	}

	void insertShow(const std::string& id, const std::string& title)
	{
		SQLite::Statement s(db.get(), "INSERT INTO show(show_id, title) VALUES (?,?)");
		s.bind(1, id);
		s.bind(2, title);
		s.exec();
	}

	void insertEpisode(const std::string& id, const std::string& show_id, int season, int episode,
					   const std::string& title = "Ep", const std::string& file_path       = "/media/e.mkv",
					   int64_t duration_ms      = 1000, const std::string& linked_movie_id = "")
	{
		SQLite::Statement s(db.get(), R"(
			INSERT INTO episode (episode_id, show_id, title, thumb, season, episode, duration_ms, file_path, linked_movie_id)
			VALUES (?, ?, ?, '', ?, ?, ?, ?, NULLIF(?, ''))
		)");
		s.bind(1, id);
		s.bind(2, show_id);
		s.bind(3, title);
		s.bind(4, season);
		s.bind(5, episode);
		s.bind(6, duration_ms);
		s.bind(7, file_path);
		s.bind(8, linked_movie_id);
		s.exec();
	}

	// Writes a watch_progress row directly (position/duration determine
	// "completed" freshly on every read — see WatchProgressRepository's own
	// comment — so no need to go through upsert()'s rewatch-counter logic
	// here, a plain INSERT is enough and keeps intent obvious per-test).
	void insertWatchProgress(const std::string& user_id, const std::string& content_type,
							 const std::string& content_id, int64_t position_ms, int64_t duration_ms,
							 int64_t updated_at = 1000)
	{
		SQLite::Statement s(db.get(), R"(
			INSERT INTO watch_progress (user_id, content_type, content_id, position_ms, duration_ms, updated_at)
			VALUES (?,?,?,?,?,?)
		)");
		s.bind(1, user_id);
		s.bind(2, content_type);
		s.bind(3, content_id);
		s.bind(4, position_ms);
		s.bind(5, duration_ms);
		s.bind(6, updated_at);
		s.exec();
	}
};

// ---------------------------------------------------------------------------
// GET /api/playback/:content_type/:id — keyframes_ms/keyframes_size/keyframes_mtime
// ---------------------------------------------------------------------------

TEST_F(PlaybackKeyframesAndResolveRoutesTest, GetPlaybackReturnsEmptyKeyframesWhenNeverProbed)
{
	insertMovie("m1", "Movie One");

	auto r = cli->Get("/api/playback/movie/m1");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	EXPECT_EQ(body["keyframes_ms"], json::array());
	EXPECT_EQ(body["keyframes_size"], 0);
	EXPECT_EQ(body["keyframes_mtime"], 0);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, GetPlaybackReturnsStoredKeyframes)
{
	insertMovie("m1", "Movie One");
	SQLite::Statement upd(db.get(),
						  "UPDATE movie SET keyframes_ms = ?, keyframes_size = ?, keyframes_mtime = ? WHERE movie_id = 'm1'");
	upd.bind(1, std::string("[0,4004,8008]"));
	upd.bind(2, int64_t{123456});
	upd.bind(3, int64_t{789});
	upd.exec();

	auto r = cli->Get("/api/playback/movie/m1");
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	EXPECT_EQ(body["keyframes_ms"], (json{0, 4004, 8008}));
	EXPECT_EQ(body["keyframes_size"], 123456);
	EXPECT_EQ(body["keyframes_mtime"], 789);
}

// Corrupt/malformed stored value must not crash the route — falls back to [].
TEST_F(PlaybackKeyframesAndResolveRoutesTest, GetPlaybackFallsBackToEmptyArrayOnCorruptStoredKeyframes)
{
	insertMovie("m1", "Movie One");
	SQLite::Statement upd(db.get(), "UPDATE movie SET keyframes_ms = ? WHERE movie_id = 'm1'");
	upd.bind(1, std::string("{not valid json at all"));
	upd.exec();

	auto r = cli->Get("/api/playback/movie/m1");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	EXPECT_EQ(body["keyframes_ms"], json::array());
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, GetPlaybackLinkedSpecialResolvesKeyframesThroughMovie)
{
	insertShow("s1", "Show One");
	insertMovie("m1", "The Special Movie", "/media/special.mkv", 3600000);
	SQLite::Statement upd(db.get(), "UPDATE movie SET keyframes_ms = ?, keyframes_size = 55, keyframes_mtime = 66 WHERE movie_id = 'm1'");
	upd.bind(1, std::string("[1,2,3]"));
	upd.exec();
	insertEpisode("e-special", "s1", 0, 1, "OVA", "", 0, "m1");

	auto r = cli->Get("/api/playback/episode/e-special");
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	EXPECT_EQ(body["keyframes_ms"], (json{1, 2, 3}));
	EXPECT_EQ(body["keyframes_size"], 55);
	EXPECT_EQ(body["keyframes_mtime"], 66);
}

// ---------------------------------------------------------------------------
// PUT /api/playback/:content_type/:id/keyframes — internal-token gate
// ---------------------------------------------------------------------------

TEST_F(PlaybackKeyframesAndResolveRoutesTest, PutKeyframesRejectsRequestWithNoInternalToken)
{
	insertMovie("m1", "Movie One");
	auto r = cli->Put("/api/playback/movie/m1/keyframes", R"({"keyframes_ms":[0,1000]})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, PutKeyframesRejectsRequestWithWrongInternalToken)
{
	insertMovie("m1", "Movie One");
	httplib::Headers headers{{"X-Internal-Token", "not-the-real-token"}};
	auto r = cli->Put("/api/playback/movie/m1/keyframes", headers, R"({"keyframes_ms":[0,1000]})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, PutKeyframesAcceptsRequestWithCorrectInternalTokenAndPersists)
{
	insertMovie("m1", "Movie One");
	auto r = cli->Put("/api/playback/movie/m1/keyframes", internalHeaders(),
					  R"({"keyframes_ms":[0,4004,8008],"size":42,"mtime":99})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);

	auto g = cli->Get("/api/playback/movie/m1");
	ASSERT_TRUE(g);
	json body = json::parse(g->body);
	EXPECT_EQ(body["keyframes_ms"], (json{0, 4004, 8008}));
	EXPECT_EQ(body["keyframes_size"], 42);
	EXPECT_EQ(body["keyframes_mtime"], 99);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, PutKeyframesRejectsInvalidContentType)
{
	auto r = cli->Put("/api/playback/show/whatever/keyframes", internalHeaders(),
					  R"({"keyframes_ms":[]})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 400);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, PutKeyframesRejectsMissingKeyframesMsField)
{
	insertMovie("m1", "Movie One");
	auto r = cli->Put("/api/playback/movie/m1/keyframes", internalHeaders(),
					  R"({"size":42})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 400);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, PutKeyframesRejectsNonArrayKeyframesMsField)
{
	insertMovie("m1", "Movie One");
	auto r = cli->Put("/api/playback/movie/m1/keyframes", internalHeaders(),
					  R"({"keyframes_ms":"not-an-array"})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 400);
}

// The show-specials-linking branch: an episode with linked_movie_id set has
// no keyframe data of its own — the PUT must write through to the linked
// MOVIE row, not the episode row, mirroring the GET route's own resolution.
TEST_F(PlaybackKeyframesAndResolveRoutesTest, PutKeyframesOnLinkedSpecialWritesToLinkedMovieRow)
{
	insertShow("s1", "Show One");
	insertMovie("m1", "The Special Movie", "/media/special.mkv", 3600000);
	insertEpisode("e-special", "s1", 0, 1, "OVA", "", 0, "m1");

	auto r = cli->Put("/api/playback/episode/e-special/keyframes", internalHeaders(),
					  R"({"keyframes_ms":[10,20,30],"size":7,"mtime":8})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);

	// The movie row got the write...
	SQLite::Statement mq(db.get(), "SELECT keyframes_ms, keyframes_size, keyframes_mtime FROM movie WHERE movie_id = 'm1'");
	ASSERT_TRUE(mq.executeStep());
	EXPECT_EQ(mq.getColumn(0).getString(), "[10,20,30]");
	EXPECT_EQ(mq.getColumn(1).getInt64(), 7);
	EXPECT_EQ(mq.getColumn(2).getInt64(), 8);

	// ...and the episode row itself was never touched.
	SQLite::Statement eq(db.get(), "SELECT keyframes_ms, keyframes_size, keyframes_mtime FROM episode WHERE episode_id = 'e-special'");
	ASSERT_TRUE(eq.executeStep());
	EXPECT_EQ(eq.getColumn(0).getString(), "");
	EXPECT_EQ(eq.getColumn(1).getInt64(), 0);
	EXPECT_EQ(eq.getColumn(2).getInt64(), 0);
}

// ---------------------------------------------------------------------------
// GET /api/shows/:id/watch-state
// ---------------------------------------------------------------------------

TEST_F(PlaybackKeyframesAndResolveRoutesTest, WatchStateRequiresAuth)
{
	auto r = cli->Get("/api/shows/s1/watch-state");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, WatchStateReturnsNullWhenNoStateAtAll)
{
	insertShow("s1", "Show One");
	auto r = cli->Get("/api/shows/s1/watch-state", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_EQ(r->body, "null");
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, WatchStateReturnsMostRecentEpisodeRegardlessOfCompleted)
{
	insertShow("s1", "Show One");
	insertEpisode("e1", "s1", 1, 1);
	insertEpisode("e2", "s1", 1, 2);
	insertWatchProgress(viewer_id, "episode", "e1", 500, 1000, 1000);
	// e2 finished (completed via fresh position>=duration) and touched later.
	insertWatchProgress(viewer_id, "episode", "e2", 1000, 1000, 2000);

	auto r = cli->Get("/api/shows/s1/watch-state", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	EXPECT_EQ(body["content_id"], "e2");
	EXPECT_EQ(body["completed"], true);
	EXPECT_EQ(body["position_ms"], 1000);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, WatchStateOnUnknownShowIdReturnsNull)
{
	auto r = cli->Get("/api/shows/does-not-exist/watch-state", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_EQ(r->body, "null");
}

// ---------------------------------------------------------------------------
// GET /api/movies/:id/resolve-play-target
// ---------------------------------------------------------------------------

TEST_F(PlaybackKeyframesAndResolveRoutesTest, MovieResolvePlayTargetRequiresAuth)
{
	auto r = cli->Get("/api/movies/m1/resolve-play-target");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, MovieResolvePlayTargetWithNoWatchProgressIsZero)
{
	insertMovie("m1", "Movie One");
	auto r = cli->Get("/api/movies/m1/resolve-play-target", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	EXPECT_EQ(body["kind"], "movie");
	EXPECT_EQ(body["id"], "m1");
	EXPECT_EQ(body["position_ms"], 0);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, MovieResolvePlayTargetInProgressResumesAtPosition)
{
	insertMovie("m1", "Movie One");
	insertWatchProgress(viewer_id, "movie", "m1", 42000, 5400000);

	auto r = cli->Get("/api/movies/m1/resolve-play-target", viewerHeaders());
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	EXPECT_EQ(body["position_ms"], 42000);
}

// A finished movie is a rewatch-in-waiting, not something that resumes at
// the very end -- resolves to position_ms=0.
TEST_F(PlaybackKeyframesAndResolveRoutesTest, MovieResolvePlayTargetCompletedResolvesToZero)
{
	insertMovie("m1", "Movie One");
	insertWatchProgress(viewer_id, "movie", "m1", 5400000, 5400000);

	auto r = cli->Get("/api/movies/m1/resolve-play-target", viewerHeaders());
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	EXPECT_EQ(body["position_ms"], 0);
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, MovieResolvePlayTargetOnUnknownIdStillResolvesToZero)
{
	// The route does no existence check on the movie row itself -- it only
	// ever looks at watch_progress, so an unknown id degrades to the same
	// "no progress" shape rather than 404ing.
	auto r = cli->Get("/api/movies/does-not-exist/resolve-play-target", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	EXPECT_EQ(body["position_ms"], 0);
	EXPECT_EQ(body["id"], "does-not-exist");
}

// ---------------------------------------------------------------------------
// GET /api/shows/:id/resolve-play-target
// ---------------------------------------------------------------------------

TEST_F(PlaybackKeyframesAndResolveRoutesTest, ShowResolvePlayTargetRequiresAuth)
{
	auto r = cli->Get("/api/shows/s1/resolve-play-target");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

// (a) In-progress (not completed) episode -> resume that episode at position.
TEST_F(PlaybackKeyframesAndResolveRoutesTest, ShowResolvePlayTargetInProgressResumesSameEpisode)
{
	insertShow("s1", "Show One");
	insertEpisode("e1", "s1", 1, 1);
	insertEpisode("e2", "s1", 1, 2);
	insertWatchProgress(viewer_id, "episode", "e1", 300, 1000);

	auto r = cli->Get("/api/shows/s1/resolve-play-target", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	EXPECT_EQ(body["kind"], "episode");
	EXPECT_EQ(body["id"], "e1");
	EXPECT_EQ(body["position_ms"], 300);
}

// (b) Completed episode with a next episode available -> that next episode at 0.
TEST_F(PlaybackKeyframesAndResolveRoutesTest, ShowResolvePlayTargetCompletedWithNextEpisodeAdvances)
{
	insertShow("s1", "Show One");
	insertEpisode("e1", "s1", 1, 1);
	insertEpisode("e2", "s1", 1, 2);
	insertWatchProgress(viewer_id, "episode", "e1", 1000, 1000);

	auto r = cli->Get("/api/shows/s1/resolve-play-target", viewerHeaders());
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	EXPECT_EQ(body["kind"], "episode");
	EXPECT_EQ(body["id"], "e2");
	EXPECT_EQ(body["position_ms"], 0);
}

// (c1) Completed episode with NO next episode (finale watched) -> falls back
// to the first episode in season/episode order, at position 0.
TEST_F(PlaybackKeyframesAndResolveRoutesTest, ShowResolvePlayTargetFinaleWatchedFallsBackToFirstEpisode)
{
	insertShow("s1", "Show One");
	insertEpisode("e1", "s1", 1, 1);
	insertEpisode("e2", "s1", 1, 2);
	insertWatchProgress(viewer_id, "episode", "e2", 1000, 1000); // the finale, fully watched

	auto r = cli->Get("/api/shows/s1/resolve-play-target", viewerHeaders());
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	EXPECT_EQ(body["kind"], "episode");
	EXPECT_EQ(body["id"], "e1");
	EXPECT_EQ(body["position_ms"], 0);
}

// (c2) No watch state at all -> same first-episode fallback.
TEST_F(PlaybackKeyframesAndResolveRoutesTest, ShowResolvePlayTargetNoWatchStateFallsBackToFirstEpisode)
{
	insertShow("s1", "Show One");
	insertEpisode("e2", "s1", 1, 2);
	insertEpisode("e1", "s1", 1, 1); // inserted out of order -- ordering must still come from season/episode

	auto r = cli->Get("/api/shows/s1/resolve-play-target", viewerHeaders());
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	EXPECT_EQ(body["kind"], "episode");
	EXPECT_EQ(body["id"], "e1");
	EXPECT_EQ(body["position_ms"], 0);
}

// An empty show (no episodes at all) returns JSON null.
TEST_F(PlaybackKeyframesAndResolveRoutesTest, ShowResolvePlayTargetEmptyShowReturnsNull)
{
	insertShow("s1", "Show One");
	auto r = cli->Get("/api/shows/s1/resolve-play-target", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_EQ(r->body, "null");
}

TEST_F(PlaybackKeyframesAndResolveRoutesTest, ShowResolvePlayTargetUnknownShowIdReturnsNull)
{
	auto r = cli->Get("/api/shows/does-not-exist/resolve-play-target", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_EQ(r->body, "null");
}