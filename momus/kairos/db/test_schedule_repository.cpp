#include <gtest/gtest.h>
#include "db/Database.h"
#include "db/ScheduleRepository.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <string>

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ScheduleRepositoryTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ScheduleRepository repo{db};

	void SetUp() override
	{
		auto& raw = db.get();
		raw.exec("INSERT INTO channel (channel_id, name, number) VALUES ('c1','Test',1)");
	}

	void addFillerEntry(const std::string& content_type, const std::string& content_id,
						int position = 0)
	{
		SQLite::Statement s(db.get(),
							"INSERT INTO channel_filler_entry (channel_id, content_type, content_id, position)"
							" VALUES ('c1',?,?,?)");
		s.bind(1, content_type);
		s.bind(2, content_id);
		s.bind(3, position);
		s.exec();
	}
};

// ---------------------------------------------------------------------------
// getChannelFillerFallback
// ---------------------------------------------------------------------------

// channel_filler_entry has carried generic content_type/content_id columns since
// migrations v35/v36 (Database.cpp) — ChannelRepository::addFillerEntry, the only
// insert path, has never populated the legacy filler_list_id column. A fallback
// query still joining on filler_list_id would silently match nothing for every
// entry added through the current API, regardless of content_type.
TEST_F(ScheduleRepositoryTest, ChannelFillerFallback_ResolvesFillerListEntry)
{
	db.get().exec("INSERT INTO filler_list (filler_list_id, title, advancement)"
		" VALUES ('fl1','Bumps','sequential')");
	db.get().exec("INSERT INTO movie (movie_id, title, file_path, duration_ms)"
		" VALUES ('m1','Movie One','/m1.mkv',600000)");
	db.get().exec("INSERT INTO filler_list_item (filler_list_id, item_type, item_id, position)"
		" VALUES ('fl1','movie','m1',0)");
	addFillerEntry("filler_list", "fl1");

	auto r = repo.getChannelFillerFallback("c1");
	ASSERT_TRUE(r.has_value());
	EXPECT_EQ(r->item_type, "movie");
	EXPECT_EQ(r->item_id, "m1");
	EXPECT_EQ(r->file_path, "/m1.mkv");
}

TEST_F(ScheduleRepositoryTest, ChannelFillerFallback_ResolvesMovieEntry)
{
	db.get().exec("INSERT INTO movie (movie_id, title, file_path, duration_ms)"
		" VALUES ('m1','Movie One','/m1.mkv',600000)");
	addFillerEntry("movie", "m1");

	auto r = repo.getChannelFillerFallback("c1");
	ASSERT_TRUE(r.has_value());
	EXPECT_EQ(r->item_type, "movie");
	EXPECT_EQ(r->item_id, "m1");
	EXPECT_EQ(r->file_path, "/m1.mkv");
}

TEST_F(ScheduleRepositoryTest, ChannelFillerFallback_ResolvesPlaylistEntry)
{
	db.get().exec("INSERT INTO playlist (playlist_id, title) VALUES ('p1','Playlist One')");
	db.get().exec("INSERT INTO movie (movie_id, title, file_path, duration_ms)"
		" VALUES ('m1','Movie One','/m1.mkv',600000)");
	db.get().exec("INSERT INTO playlist_item (playlist_id, item_type, item_id, position)"
		" VALUES ('p1','movie','m1',0)");
	addFillerEntry("playlist", "p1");

	auto r = repo.getChannelFillerFallback("c1");
	ASSERT_TRUE(r.has_value());
	EXPECT_EQ(r->item_type, "movie");
	EXPECT_EQ(r->item_id, "m1");
}

TEST_F(ScheduleRepositoryTest, ChannelFillerFallback_ResolvesShowEntryHonoringSeasonFilter)
{
	db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1','Show One')");
	db.get().exec("INSERT INTO episode (episode_id, show_id, season, episode, title, file_path, duration_ms)"
		" VALUES ('e1','s1',1,1,'E1','/e1.mkv',600000)");
	db.get().exec("INSERT INTO episode (episode_id, show_id, season, episode, title, file_path, duration_ms)"
		" VALUES ('e2','s1',2,1,'E2','/e2.mkv',600000)");
	SQLite::Statement s(db.get(),
						"INSERT INTO channel_filler_entry (channel_id, content_type, content_id, position, season_filter)"
						" VALUES ('c1','show','s1',0,1)");
	s.exec();

	auto r = repo.getChannelFillerFallback("c1");
	ASSERT_TRUE(r.has_value());
	EXPECT_EQ(r->item_type, "episode");
	EXPECT_EQ(r->item_id, "e1"); // season 2 (e2) excluded by season_filter=1
}

TEST_F(ScheduleRepositoryTest, ChannelFillerFallback_NulloptWhenNoEntriesConfigured)
{
	EXPECT_FALSE(repo.getChannelFillerFallback("c1").has_value());
}

TEST_F(ScheduleRepositoryTest, ChannelFillerFallback_SkipsEntryWithNoFileOnRecord)
{
	// Movie row exists but file_path is empty (e.g. source unmounted) — should be
	// excluded, not returned with a blank path for Hephaestus to fail on.
	db.get().exec("INSERT INTO movie (movie_id, title, file_path, duration_ms)"
		" VALUES ('m1','Movie One','',600000)");
	addFillerEntry("movie", "m1");

	EXPECT_FALSE(repo.getChannelFillerFallback("c1").has_value());
}