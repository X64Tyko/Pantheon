#include <gtest/gtest.h>
#include "db/Database.h"
#include "db/ContentRepository.h"
#include "conf/ConfStore.h"
#include "scraper/ScraperManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <filesystem>

class ScraperManagerTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	// Use a temp file for ConfStore to avoid creating accidental ":memory:" files in the cwd.
	std::string conf_path = (std::filesystem::temp_directory_path() / "momus_conf_test").string();
	ConfStore conf{conf_path};
	ScraperManager manager{db, conf};
	ContentRepository repo{db};

	void SetUp() override
	{
	}

	void TearDown() override
	{
		std::filesystem::remove(conf_path);
	}

	void insertShow(const std::string& id, const std::string& title, const std::string& thumb = "", const std::string& art = "")
	{
		SQLite::Statement s(db.get(), "INSERT INTO show (show_id, title, thumb, art) VALUES (?,?,?,?)");
		s.bind(1, id);
		s.bind(2, title);
		s.bind(3, thumb);
		s.bind(4, art);
		s.exec();
	}

	void insertMovie(const std::string& id, const std::string& title, const std::string& thumb = "", const std::string& art = "")
	{
		SQLite::Statement s(db.get(), "INSERT INTO movie (movie_id, title, file_path, duration_ms, thumb, art) VALUES (?,?,'/tmp/movie',0,?,?)");
		s.bind(1, id);
		s.bind(2, title);
		s.bind(3, thumb);
		s.bind(4, art);
		s.exec();
	}
};

TEST_F(ScraperManagerTest, ImagePersistenceShow)
{
	insertShow("s1", "Show 1", "old_thumb.jpg", "old_art.jpg");

	// Verify initial state
	auto thumb = repo.getShowThumb("s1");
	ASSERT_TRUE(thumb.has_value());
	EXPECT_EQ(thumb->image_path, "old_thumb.jpg");

	// Manually update images via SQL to simulate what refreshMetadata does
	// (since mocking ScraperManager internal scrapers is complex in this integration test)
	SQLite::Statement update(db.get(), "UPDATE show SET thumb = ?, art = ? WHERE show_id = ?");
	update.bind(1, "new_thumb.jpg");
	update.bind(2, "new_art.jpg");
	update.bind(3, "s1");
	update.exec();

	// Verify repository retrieves updated images
	auto new_thumb = repo.getShowThumb("s1");
	ASSERT_TRUE(new_thumb.has_value());
	EXPECT_EQ(new_thumb->image_path, "new_thumb.jpg");

	auto new_art = repo.getShowArt("s1");
	ASSERT_TRUE(new_art.has_value());
	EXPECT_EQ(new_art->image_path, "new_art.jpg");
}

TEST_F(ScraperManagerTest, ImagePersistenceMovie)
{
	insertMovie("m1", "Movie 1", "m_old_thumb.jpg", "m_old_art.jpg");

	auto thumb = repo.getMovieThumb("m1");
	ASSERT_TRUE(thumb.has_value());
	EXPECT_EQ(thumb->image_path, "m_old_thumb.jpg");

	SQLite::Statement update(db.get(), "UPDATE movie SET thumb = ?, art = ? WHERE movie_id = ?");
	update.bind(1, "m_new_thumb.jpg");
	update.bind(2, "m_new_art.jpg");
	update.bind(3, "m1");
	update.exec();

	auto new_thumb = repo.getMovieThumb("m1");
	ASSERT_TRUE(new_thumb.has_value());
	EXPECT_EQ(new_thumb->image_path, "m_new_thumb.jpg");

	auto new_art = repo.getMovieArt("m1");
	ASSERT_TRUE(new_art.has_value());
	EXPECT_EQ(new_art->image_path, "m_new_art.jpg");
}

TEST_F(ScraperManagerTest, ImagePersistenceEpisode)
{
	insertShow("s1", "Show 1");
	SQLite::Statement s(db.get(), "INSERT INTO episode (episode_id, show_id, title, thumb, season, episode, duration_ms, file_path) VALUES ('e1', 's1', 'Ep 1', 'e_old_thumb.jpg', 1, 1, 0, '/tmp/ep')");
	s.exec();

	auto thumb = repo.getEpisodeThumb("e1");
	ASSERT_TRUE(thumb.has_value());
	EXPECT_EQ(thumb->image_path, "e_old_thumb.jpg");

	SQLite::Statement update(db.get(), "UPDATE episode SET thumb = ? WHERE episode_id = ?");
	update.bind(1, "e_new_thumb.jpg");
	update.bind(2, "e1");
	update.exec();

	auto new_thumb = repo.getEpisodeThumb("e1");
	ASSERT_TRUE(new_thumb.has_value());
	EXPECT_EQ(new_thumb->image_path, "e_new_thumb.jpg");
}

TEST_F(ScraperManagerTest, SetAndGetExternalIds)
{
	insertShow("s1", "Show 1");

	std::vector<ScraperManager::ExternalId> ids = {
		{"tmdb", "123", 1},
		{"tvdb", "456", 2}
	};

	manager.setExternalIds("s1", "show", ids);

	auto loaded = manager.getExternalIds("s1", "show");
	ASSERT_EQ(loaded.size(), 2u);
	EXPECT_EQ(loaded[0].source, "tmdb");
	EXPECT_EQ(loaded[0].external_id, "123");
	EXPECT_EQ(loaded[0].priority, 1);
	EXPECT_EQ(loaded[1].source, "tvdb");
	EXPECT_EQ(loaded[1].external_id, "456");
	EXPECT_EQ(loaded[1].priority, 2);
}

TEST_F(ScraperManagerTest, AlternateTitles)
{
	insertShow("s1", "Show 1");

	std::vector<ScraperManager::AlternateTitle> titles = {
		{"en", "Title A", "An overview"},
		{"es", "Title B", ""},
	};
	manager.setAlternateTitles("s1", "show", titles);

	auto loaded = manager.getAlternateTitles("s1", "show");
	ASSERT_EQ(loaded.size(), 2u);
	auto has = [&](const std::string& title)
	{
		return std::find_if(loaded.begin(), loaded.end(),
							[&](const auto& t) { return t.title == title; }) != loaded.end();
	};
	EXPECT_TRUE(has("Title A"));
	EXPECT_TRUE(has("Title B"));

	auto titleB = std::find_if(loaded.begin(), loaded.end(),
							   [](const auto& t) { return t.title == "Title B"; });
	ASSERT_NE(titleB, loaded.end());
	EXPECT_EQ(titleB->language, "es");
}

TEST_F(ScraperManagerTest, PriorityOrderingOnUpdate)
{
	insertShow("s1", "Show 1");

	// Initial set - testing that it sorts by priority ASC
	manager.setExternalIds("s1", "show", {{"tvdb", "456", 2}, {"tmdb", "123", 1}});

	// Check they are returned sorted by priority
	auto loaded = manager.getExternalIds("s1", "show");
	ASSERT_EQ(loaded.size(), 2u);
	EXPECT_EQ(loaded[0].source, "tmdb"); // priority 1
	EXPECT_EQ(loaded[1].source, "tvdb"); // priority 2
}

TEST_F(ScraperManagerTest, MovieExternalIds)
{
	insertMovie("m1", "Movie 1");

	manager.setExternalIds("m1", "movie", {{"tmdb", "m123", 1}});
	auto loaded = manager.getExternalIds("m1", "movie");
	ASSERT_EQ(loaded.size(), 1u);
	EXPECT_EQ(loaded[0].source, "tmdb");
}

TEST_F(ScraperManagerTest, LanguageWeightSettings)
{
	ScraperSettings s = manager.getSettings();

	// Find TMDB config
	auto it = std::find_if(s.configs.begin(), s.configs.end(), [](const auto& c) { return c.source == "tmdb"; });
	ASSERT_NE(it, s.configs.end());

	it->language_weight = 0.5;
	it->enabled         = true;
	manager.updateSettings(s);

	ScraperSettings s2 = manager.getSettings();
	auto it2           = std::find_if(s2.configs.begin(), s2.configs.end(), [](const auto& c) { return c.source == "tmdb"; });
	// std::stod(std::to_string(0.5)) might have slight precision diff, but 0.5 is exact in float
	EXPECT_NEAR(it2->language_weight, 0.5, 0.0001);
	EXPECT_TRUE(it2->enabled);
}

// ============================================================================
// unconfirmMatch / unconfirmAllMatches — the undo path for confirmMatch/
// confirmAllMatches, added so an accidental bulk-confirm can be reversed.
// ============================================================================

TEST_F(ScraperManagerTest, UnconfirmMatch_ClearsConfirmedFlagButKeepsMatchStatus)
{
	insertMovie("m1", "Movie 1");
	SQLite::Statement upd(db.get(), "UPDATE movie SET match_status='matched', match_confirmed=1 WHERE movie_id='m1'");
	upd.exec();

	EXPECT_TRUE(manager.unconfirmMatch("movie", "m1"));

	SQLite::Statement q(db.get(), "SELECT match_status, match_confirmed FROM movie WHERE movie_id='m1'");
	ASSERT_TRUE(q.executeStep());
	EXPECT_EQ(q.getColumn(0).getString(), "matched");
	EXPECT_EQ(q.getColumn(1).getInt(), 0);
}

TEST_F(ScraperManagerTest, UnconfirmMatch_FalseWhenNotCurrentlyConfirmed)
{
	insertMovie("m1", "Movie 1"); // match_confirmed defaults to 0
	EXPECT_FALSE(manager.unconfirmMatch("movie", "m1"));
}

TEST_F(ScraperManagerTest, UnconfirmMatch_FalseForInvalidItemType)
{
	insertMovie("m1", "Movie 1");
	EXPECT_FALSE(manager.unconfirmMatch("episode", "m1"));
}

TEST_F(ScraperManagerTest, UnconfirmAllMatches_ClearsEveryConfirmedItemOnly)
{
	insertMovie("m1", "Movie 1");
	insertMovie("m2", "Movie 2");
	insertShow("s1", "Show 1");
	SQLite::Statement upd(db.get(),
						  "UPDATE movie SET match_status='matched', match_confirmed=1 WHERE movie_id='m1';");
	upd.exec();
	SQLite::Statement upd2(db.get(),
						   "UPDATE show SET match_status='matched', match_confirmed=1 WHERE show_id='s1'");
	upd2.exec();
	// m2 stays unconfirmed throughout.

	EXPECT_EQ(manager.unconfirmAllMatches(), 2);

	SQLite::Statement q1(db.get(), "SELECT match_confirmed FROM movie WHERE movie_id='m1'");
	ASSERT_TRUE(q1.executeStep());
	EXPECT_EQ(q1.getColumn(0).getInt(), 0);

	SQLite::Statement q2(db.get(), "SELECT match_confirmed FROM show WHERE show_id='s1'");
	ASSERT_TRUE(q2.executeStep());
	EXPECT_EQ(q2.getColumn(0).getInt(), 0);

	SQLite::Statement q3(db.get(), "SELECT match_confirmed FROM movie WHERE movie_id='m2'");
	ASSERT_TRUE(q3.executeStep());
	EXPECT_EQ(q3.getColumn(0).getInt(), 0); // was already 0 — untouched, not miscounted

	// Re-running with nothing left confirmed is a no-op.
	EXPECT_EQ(manager.unconfirmAllMatches(), 0);
}

// ============================================================================
// nfo_confirmed — restoring match_confirmed (not just the match itself) via
// the trusted-ID short-circuit in matchShow()/matchMovie(), for an item whose
// confirmed match was previously written back to its on-disk NFO. See
// SidecarMetadata.h / LocalSource::pushMetadata.
// ============================================================================

TEST_F(ScraperManagerTest, MatchShow_TrustedId_NfoConfirmedRestoresMatchConfirmed)
{
	// No episode rows -> matchShow()'s folder-agreement check is skipped
	// entirely (has_paths stays false), so the trusted tmdb_id is accepted
	// unconditionally — exactly the "fresh DB, first sync after a wipe" case.
	SQLite::Statement ins(db.get(),
						  "INSERT INTO show (show_id, title, tmdb_id, match_status, nfo_confirmed) "
						  "VALUES ('s1','Breaking Bad','1396','unscraped',1)");
	ins.exec();

	manager.runMatchSync("s1", "show");

	SQLite::Statement q(db.get(), "SELECT match_status, match_confirmed FROM show WHERE show_id='s1'");
	ASSERT_TRUE(q.executeStep());
	EXPECT_EQ(q.getColumn(0).getString(), "matched");
	EXPECT_EQ(q.getColumn(1).getInt(), 1);
}

TEST_F(ScraperManagerTest, MatchShow_TrustedId_WithoutNfoConfirmed_LeavesMatchUnconfirmed)
{
	SQLite::Statement ins(db.get(),
						  "INSERT INTO show (show_id, title, tmdb_id, match_status, nfo_confirmed) "
						  "VALUES ('s1','Breaking Bad','1396','unscraped',0)");
	ins.exec();

	manager.runMatchSync("s1", "show");

	SQLite::Statement q(db.get(), "SELECT match_status, match_confirmed FROM show WHERE show_id='s1'");
	ASSERT_TRUE(q.executeStep());
	EXPECT_EQ(q.getColumn(0).getString(), "matched");
	EXPECT_EQ(q.getColumn(1).getInt(), 0);
}

TEST_F(ScraperManagerTest, MatchMovie_TrustedId_NfoConfirmedRestoresMatchConfirmed)
{
	// Empty file_path -> matchMovie()'s folder-agreement check is skipped
	// entirely, same "fresh DB" reasoning as the show case above.
	SQLite::Statement ins(db.get(),
						  "INSERT INTO movie (movie_id, title, file_path, duration_ms, tmdb_id, match_status, nfo_confirmed) "
						  "VALUES ('m1','The Matrix','',0,'603','unscraped',1)");
	ins.exec();

	manager.runMatchSync("m1", "movie");

	SQLite::Statement q(db.get(), "SELECT match_status, match_confirmed FROM movie WHERE movie_id='m1'");
	ASSERT_TRUE(q.executeStep());
	EXPECT_EQ(q.getColumn(0).getString(), "matched");
	EXPECT_EQ(q.getColumn(1).getInt(), 1);
}

TEST_F(ScraperManagerTest, UnconfirmMatch_NotReappliedByAResyncThatLeavesNfoConfirmedSet)
{
	// Simulates: item was restored confirmed via nfo_confirmed, a human then
	// used Unconfirm on it, and a later sync pass runs again while the item's
	// on-disk NFO still says confirmed (nothing deleted that tag). Because
	// matchShow/matchMovie only ever run for non-'matched' rows, and this row
	// stays 'matched' throughout, the un-confirm must stick.
	SQLite::Statement ins(db.get(),
						  "INSERT INTO movie (movie_id, title, file_path, duration_ms, tmdb_id, match_status, match_confirmed, nfo_confirmed) "
						  "VALUES ('m1','The Matrix','',0,'603','matched',1,1)");
	ins.exec();

	EXPECT_TRUE(manager.unconfirmMatch("movie", "m1"));

	// A resync would re-run the SAME upsert (nfo_confirmed stays 1 on disk),
	// but runMatch()/matchMovie() itself is never invoked for a row that's
	// already 'matched' — modelled here by calling it anyway and confirming
	// it's a no-op precisely because match_status isn't one of the pending
	// values matchMovie's caller selects on.
	manager.runMatchSync("m1", "movie");

	SQLite::Statement q(db.get(), "SELECT match_status, match_confirmed FROM movie WHERE movie_id='m1'");
	ASSERT_TRUE(q.executeStep());
	EXPECT_EQ(q.getColumn(0).getString(), "matched");
	EXPECT_EQ(q.getColumn(1).getInt(), 0);
}