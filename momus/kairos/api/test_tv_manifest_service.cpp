// Coverage for GET /api/tv/manifest (TvManifestService.cpp) — previously
// untested entirely. Two things this file locks in:
//
//   * The static tv_shelf/tv_zone rows seeded by Database.cpp's v81
//     migration render into the expected hero/shelf/guide shapes.
//   * Playlist-backed home shelves (a smart playlist with show_on_home=1,
//     see PlaylistRepository::listHomeShelves) are appended as additional
//     shelf rows after every static tv_shelf row, in listHomeShelves' own
//     relative order, with the "playlist-<id>" id, the fixed
//     itemAction/endTile/emptyBehavior, and dataSource.endpoint switched on
//     smart_type (movie -> /api/movies, show -> /api/shows).
//
// Same fixture shape as the other api/test_*_routes.cpp files: a real
// Router wired to an in-memory Database, a real httplib::Server on a test
// port, plain HTTP calls via httplib::Client. The route is unauthenticated
// (see Router.cpp's isPublicPath — "structural data, not per-user"), so no
// auth setup is needed here at all.

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
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
		std::error_code ec;
		std::filesystem::remove("./momus_tv_manifest_test.conf", ec);
	}

	// Mirrors momus/kairos/db/test_playlist_repository.cpp's own helpers so
	// playlist rows are seeded the exact same way that file already does.
	std::string makeSmartHomeShelf(const std::string& title, const std::string& smart_type,
								   const std::string& filter_expr, int home_order = 0)
	{
		auto playlist_id = playlists.create(title);
		SQLite::Statement s(db.get(), R"(
			UPDATE playlist SET membership='smart', smart_type=?, filter_expr=?, smart_sort='title',
			                     show_on_home=1, home_order=?
			WHERE playlist_id = ?
		)");
		s.bind(1, smart_type);
		s.bind(2, filter_expr);
		s.bind(3, home_order);
		s.bind(4, playlist_id);
		s.exec();
		return playlist_id;
	}

	json fetchManifest()
	{
		auto r = cli->Get("/api/tv/manifest");
		EXPECT_TRUE(r);
		EXPECT_EQ(r->status, 200);
		return json::parse(r->body);
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
	ASSERT_TRUE(hero.contains("dataSources"));
	EXPECT_TRUE(hero["dataSources"].contains("shows"));
	EXPECT_TRUE(hero["dataSources"].contains("movies"));

	auto guide = findRow(rows, "guide");
	ASSERT_FALSE(guide.is_null());
	EXPECT_EQ(guide["type"], "guide");
	EXPECT_EQ(guide["order"], 6);
}

TEST_F(TvManifestServiceTest, StaticShelfRowShapeIsUnaffected)
{
	auto manifest = fetchManifest();
	auto rows     = manifest["home"]["rows"];

	auto shelf = findRow(rows, "recent-movies");
	ASSERT_FALSE(shelf.is_null());
	EXPECT_EQ(shelf["type"], "shelf");
	EXPECT_EQ(shelf["title"], "Recently Added Movies");
	EXPECT_EQ(shelf["dataSource"]["endpoint"], "/api/movies");
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
	EXPECT_EQ(row["dataSource"]["endpoint"], "/api/movies");
	EXPECT_EQ(row["dataSource"]["params"]["filter"], "genre:Horror");
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

TEST_F(TvManifestServiceTest, ShowTypeSmartPlaylistPointsAtShowsEndpoint)
{
	auto playlist_id = makeSmartHomeShelf("My Show Shelf", "show", "genre:Comedy");

	auto manifest = fetchManifest();
	auto row      = findRow(manifest["home"]["rows"], "playlist-" + playlist_id);
	ASSERT_FALSE(row.is_null());
	EXPECT_EQ(row["dataSource"]["endpoint"], "/api/shows");
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