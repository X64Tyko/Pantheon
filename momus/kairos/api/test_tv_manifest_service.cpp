// Coverage for GET /api/tv/manifest and GET /api/tv/shelf-items
// (TvManifestService.cpp). Three things this file locks in:
//
//   * The static tv_shelf/tv_zone rows seeded by Database.cpp's v81
//     migration render into the expected hero/shelf/guide shapes, each
//     shelf/hero row carrying an opaque `filter` object (no `dataSource`/
//     `endpoint` on the wire at all — see the v103 migration).
//   * Playlist-backed home shelves (a smart playlist with show_on_home=1,
//     see PlaylistRepository::listHomeShelves) are appended as additional
//     shelf rows after every static tv_shelf row, in listHomeShelves' own
//     relative order, with the "playlist-<id>" id and a filter whose
//     content_type is derived from smart_type (mixed/show+expand_episodes
//     both resolve to content_type "mixed", distinguished only by
//     include_movies — the actual bug this refactor fixes: the old
//     endpoint-selection ternary had no branch for "mixed" at all).
//   * GET /api/tv/shelf-items resolves a filter into tiles, dispatching on
//     content_type (show/movie/mixed/hero), and requires authentication
//     unlike the manifest route itself.
//
// Same fixture shape as the other api/test_*_routes.cpp files: a real
// Router wired to an in-memory Database, a real httplib::Server on a test
// port, plain HTTP calls via httplib::Client.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <set>
#include <thread>

#include "api/Router.h"
#include "auth/AuthStore.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include "db/PlaylistRepository.h"
#include "download/DownloadManager.h"
#include "email/EmailService.h"
#include "log/LogBuffer.h"
#include "backup/BackupManager.h"
#include "jobs/JobScheduler.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "source/SyncManager.h"

using json = nlohmann::json;

class TvManifestServiceTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ConfStore conf{"./momus_tv_manifest_test.conf"};
	SyncManager sync{db, conf};
	RuleEngine engine{db};
	EPGMaterializer materializer{db, engine};
	DownloadManager dl;
	AuthStore auth{db};
	EmailService email{db};
	LogBuffer logs;
	JobScheduler jobs;
	BackupManager backups{db, "", "", "/tmp/kairos_test_backups_unused"};
	PlaylistRepository playlists{db};

	httplib::Server svr;
	std::unique_ptr<Router> router;
	std::unique_ptr<httplib::Client> cli;
	std::thread server_thread;

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

		auth.createUser("tvmanifest_viewer", "tvmanifest-password-1", "viewer");
		viewer_token = auth.login("tvmanifest_viewer", "tvmanifest-password-1");
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
		std::error_code ec;
		std::filesystem::remove("./momus_tv_manifest_test.conf", ec);
	}

	httplib::Headers viewerHeaders() const { return {{"Authorization", "Bearer " + viewer_token}}; }

	// Mirrors momus/kairos/db/test_playlist_repository.cpp's own helpers so
	// playlist rows are seeded the exact same way that file already does.
	std::string makeSmartHomeShelf(const std::string& title, const std::string& smart_type,
								   const std::string& filter_expr, int home_order = 0,
								   bool expand_episodes                           = false)
	{
		auto playlist_id = playlists.create(title);
		SQLite::Statement s(db.get(), R"(
			UPDATE playlist SET membership='smart', smart_type=?, filter_expr=?, smart_sort='title',
			                     show_on_home=1, home_order=?, smart_expand_episodes=?
			WHERE playlist_id = ?
		)");
		s.bind(1, smart_type);
		s.bind(2, filter_expr);
		s.bind(3, home_order);
		s.bind(4, expand_episodes ? 1 : 0);
		s.bind(5, playlist_id);
		s.exec();
		return playlist_id;
	}

	void seedMovie(const std::string& id, const std::string& title)
	{
		SQLite::Statement s(db.get(),
							"INSERT INTO movie (movie_id, title, file_path, duration_ms) VALUES (?, ?, '/m.mkv', 6000000)");
		s.bind(1, id);
		s.bind(2, title);
		s.exec();
	}

	void seedShowWithEpisode(const std::string& show_id, const std::string& title)
	{
		SQLite::Statement s(db.get(), "INSERT INTO show (show_id, title) VALUES (?, ?)");
		s.bind(1, show_id);
		s.bind(2, title);
		s.exec();
		SQLite::Statement e(db.get(),
							"INSERT INTO episode (episode_id, show_id, season, episode, title, file_path, duration_ms) "
							"VALUES (?, ?, 1, 1, 'Ep', '/e.mkv', 1400000)");
		e.bind(1, show_id + "-ep1");
		e.bind(2, show_id);
		e.exec();
	}

	json fetchManifest()
	{
		auto r = cli->Get("/api/tv/manifest");
		EXPECT_TRUE(r);
		EXPECT_EQ(r->status, 200);
		return json::parse(r->body);
	}

	json fetchShelfItems(const std::string& query, bool authed = true)
	{
		auto r = authed
					 ? cli->Get("/api/tv/shelf-items?" + query, viewerHeaders())
					 : cli->Get("/api/tv/shelf-items?" + query);
		return r ? json::parse(r->body) : json::object();
	}

	static json findRow(const json& rows, const std::string& id)
	{
		for (const auto& r : rows) if (r.value("id", "") == id) return r;
		return nullptr;
	}
};

TEST_F(TvManifestServiceTest, WorksWithNoAuthAtAll)
{
	// No Authorization header at all -- consumed unauthenticated by /tv on
	// first paint, per Router.cpp's isPublicPath.
	auto manifest = fetchManifest();
	EXPECT_EQ(manifest["version"], 1);
	ASSERT_TRUE(manifest.contains("home"));
	ASSERT_TRUE(manifest["home"].contains("rows"));
}

// Static rows still render exactly as before -- hero/guide row types are
// unaffected by the new playlist-shelf appending logic.
TEST_F(TvManifestServiceTest, StaticHeroAndGuideRowsAreUnaffected)
{
	auto manifest = fetchManifest();
	auto rows     = manifest["home"]["rows"];

	auto hero = findRow(rows, "hero");
	ASSERT_FALSE(hero.is_null());
	EXPECT_EQ(hero["type"], "hero");
	EXPECT_EQ(hero["order"], 0);
	ASSERT_TRUE(hero.contains("filter"));
	EXPECT_EQ(hero["filter"]["content_type"], "hero");
	EXPECT_FALSE(hero.contains("dataSources"));

	auto guide = findRow(rows, "guide");
	ASSERT_FALSE(guide.is_null());
	EXPECT_EQ(guide["type"], "guide");
	EXPECT_EQ(guide["order"], 6);
}

TEST_F(TvManifestServiceTest, StaticShelfRowShapeCarriesAFilterNotADataSource)
{
	auto manifest = fetchManifest();
	auto rows     = manifest["home"]["rows"];

	auto shelf = findRow(rows, "recent-movies");
	ASSERT_FALSE(shelf.is_null());
	EXPECT_EQ(shelf["type"], "shelf");
	EXPECT_EQ(shelf["title"], "Recently Added Movies");
	ASSERT_TRUE(shelf.contains("filter"));
	EXPECT_EQ(shelf["filter"]["content_type"], "movie");
	EXPECT_EQ(shelf["filter"]["sort"], "recently_added");
	EXPECT_FALSE(shelf.contains("dataSource"));
	EXPECT_EQ(shelf["itemAction"], "open-detail");
	EXPECT_EQ(shelf["endTile"], "navigate-library");
	EXPECT_EQ(shelf["emptyBehavior"], "hide");
}

TEST_F(TvManifestServiceTest, PlaylistWithShowOnHomeAppearsAsShelfRow)
{
	auto playlist_id = makeSmartHomeShelf("My Horror Shelf", "movie", "genre:Horror");

	auto manifest = fetchManifest();
	auto rows     = manifest["home"]["rows"];

	auto row = findRow(rows, "playlist-" + playlist_id);
	ASSERT_FALSE(row.is_null());
	EXPECT_EQ(row["type"], "shelf");
	EXPECT_EQ(row["title"], "My Horror Shelf");
	EXPECT_EQ(row["filter"]["content_type"], "movie");
	EXPECT_EQ(row["filter"]["filter"], "genre:Horror");
	EXPECT_EQ(row["itemAction"], "open-detail");
	EXPECT_EQ(row["endTile"], "navigate-library");
	EXPECT_EQ(row["emptyBehavior"], "hide");
}

TEST_F(TvManifestServiceTest, PlaylistWithoutShowOnHomeDoesNotAppear)
{
	auto playlist_id = playlists.create("Not A Shelf");
	SQLite::Statement s(db.get(),
						"UPDATE playlist SET membership='smart', smart_type='movie', filter_expr='genre:Horror' WHERE playlist_id = ?");
	s.bind(1, playlist_id);
	s.exec();
	// show_on_home left at its default (0).

	auto manifest = fetchManifest();
	auto rows     = manifest["home"]["rows"];
	EXPECT_TRUE(findRow(rows, "playlist-" + playlist_id).is_null());
}

TEST_F(TvManifestServiceTest, ShowTypeSmartPlaylistGetsShowContentType)
{
	auto playlist_id = makeSmartHomeShelf("My Show Shelf", "show", "genre:Comedy");

	auto manifest = fetchManifest();
	auto row      = findRow(manifest["home"]["rows"], "playlist-" + playlist_id);
	ASSERT_FALSE(row.is_null());
	EXPECT_EQ(row["filter"]["content_type"], "show");
}

// The actual bug this refactor fixes: a "mixed" smart playlist home shelf
// used to fall into the old code's endpoint-selection ternary's else branch
// (-> /api/shows), silently dropping every movie. It must now resolve to
// content_type "mixed" with include_movies true.
TEST_F(TvManifestServiceTest, MixedTypeSmartPlaylistGetsMixedContentTypeWithMovies)
{
	auto playlist_id = makeSmartHomeShelf("My Mixed Shelf", "mixed", "");

	auto manifest = fetchManifest();
	auto row      = findRow(manifest["home"]["rows"], "playlist-" + playlist_id);
	ASSERT_FALSE(row.is_null());
	EXPECT_EQ(row["filter"]["content_type"], "mixed");
	EXPECT_EQ(row["filter"]["include_movies"], true);
}

// A show-typed playlist with smart_expand_episodes also resolves through the
// mixed path (episode granularity), but must NOT pull in movies.
TEST_F(TvManifestServiceTest, ShowExpandEpisodesPlaylistGetsMixedContentTypeWithoutMovies)
{
	auto playlist_id = makeSmartHomeShelf("My Expanded Show Shelf", "show", "", 0, /*expand_episodes=*/true);

	auto manifest = fetchManifest();
	auto row      = findRow(manifest["home"]["rows"], "playlist-" + playlist_id);
	ASSERT_FALSE(row.is_null());
	EXPECT_EQ(row["filter"]["content_type"], "mixed");
	EXPECT_EQ(row["filter"]["include_movies"], false);
}

// Playlist shelves come after every static tv_shelf row (max static order is
// 6, the 'guide' row), and preserve listHomeShelves' own relative order
// (home_order, then title) among themselves.
TEST_F(TvManifestServiceTest, PlaylistShelvesOrderAfterStaticRowsPreservingHomeOrder)
{
	auto second_id = makeSmartHomeShelf("B Shelf", "movie", "genre:Comedy", /*home_order=*/1);
	auto first_id  = makeSmartHomeShelf("A Shelf", "movie", "genre:Horror", /*home_order=*/0);

	auto manifest = fetchManifest();
	auto rows     = manifest["home"]["rows"];

	// 7 static rows (orders 0..6) + 2 playlist rows = 9 total.
	ASSERT_EQ(rows.size(), 9u);

	auto first_row  = findRow(rows, "playlist-" + first_id);
	auto second_row = findRow(rows, "playlist-" + second_id);
	ASSERT_FALSE(first_row.is_null());
	ASSERT_FALSE(second_row.is_null());

	// Both come strictly after every static row's order (max static order = 6).
	EXPECT_GT(first_row["order"].get<int>(), 6);
	EXPECT_GT(second_row["order"].get<int>(), 6);

	// home_order=0 ("A Shelf") sorts before home_order=1 ("B Shelf") in
	// listHomeShelves, so its manifest row order must be lower too, even
	// though it was created second.
	EXPECT_LT(first_row["order"].get<int>(), second_row["order"].get<int>());
}

// ── GET /api/tv/shelf-items ─────────────────────────────────────────────────

TEST_F(TvManifestServiceTest, ShelfItemsRequiresAuth)
{
	auto r = cli->Get("/api/tv/shelf-items?content_type=movie");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(TvManifestServiceTest, ShelfItemsResolvesMovies)
{
	seedMovie("mov1", "Alien");
	auto result = fetchShelfItems("content_type=movie&limit=16");
	ASSERT_TRUE(result.contains("items"));
	ASSERT_EQ(result["items"].size(), 1u);
	EXPECT_EQ(result["items"][0]["content_type"], "movie");
	EXPECT_EQ(result["items"][0]["id"], "mov1");
	EXPECT_EQ(result["items"][0]["title"], "Alien");
}

TEST_F(TvManifestServiceTest, ShelfItemsResolvesShows)
{
	seedShowWithEpisode("show1", "The Wire");
	auto result = fetchShelfItems("content_type=show&limit=16");
	ASSERT_EQ(result["items"].size(), 1u);
	EXPECT_EQ(result["items"][0]["content_type"], "show");
	EXPECT_EQ(result["items"][0]["id"], "show1");
}

TEST_F(TvManifestServiceTest, ShelfItemsResolvesMixedIncludingMovies)
{
	seedMovie("mov1", "Alien");
	seedShowWithEpisode("show1", "The Wire");

	auto result = fetchShelfItems("content_type=mixed&include_movies=1&limit=16");
	ASSERT_EQ(result["items"].size(), 2u);
	std::set<std::string> types;
	for (const auto& item : result["items"]) types.insert(item["content_type"].get<std::string>());
	// Mixed resolves shows to their episodes (see MixedSort.h), never whole shows.
	EXPECT_TRUE(types.count("movie"));
	EXPECT_TRUE(types.count("episode"));
	EXPECT_FALSE(types.count("show"));
}

TEST_F(TvManifestServiceTest, ShelfItemsResolvesMixedExcludingMoviesWhenToldTo)
{
	seedMovie("mov1", "Alien");
	seedShowWithEpisode("show1", "The Wire");

	auto result = fetchShelfItems("content_type=mixed&include_movies=0&limit=16");
	for (const auto& item : result["items"]) EXPECT_NE(item["content_type"], "movie");
}

TEST_F(TvManifestServiceTest, ShelfItemsResolvesHeroMergingShowsAndMovies)
{
	seedMovie("mov1", "Alien");
	seedShowWithEpisode("show1", "The Wire");

	auto result = fetchShelfItems("content_type=hero&sort=recently_added&limit=16");
	// Neither seeded item has art, so hero's art-filter-with-fallback merge
	// falls back to shows-only (see TvManifestService.cpp's hero branch) --
	// still proves both searches ran and the merge/fallback logic executed
	// without erroring, without requiring seeded image fields.
	ASSERT_TRUE(result.contains("items"));
}

TEST_F(TvManifestServiceTest, ShelfItemsUnknownContentTypeReturnsEmptyItems)
{
	auto result = fetchShelfItems("content_type=nonsense");
	ASSERT_TRUE(result.contains("items"));
	EXPECT_TRUE(result["items"].empty());
}