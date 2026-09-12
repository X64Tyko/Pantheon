// Tests for SyncManager's cross-source merge policy: source priority decides
// which source's data wins a field-level conflict, a strictly lower-priority
// source only backfills fields the current owner left empty (never locked
// out entirely — see the removed cross_ref_shows/is_cross_ref upsert gate),
// and loadSources() orders sources by sync_priority so a full sync actually
// visits them in that order.
//
// Uses two real LocalSource-backed media_source rows against real temp
// directories (with real movie.nfo sidecars) rather than mocking IMediaSource
// — this is the same dedup path (title+year cross-source match) a real
// multi-source library hits, not a synthetic shortcut around it.

#include <gtest/gtest.h>
#include "db/Database.h"
#include "conf/ConfStore.h"
#include "source/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

class SyncManagerTest : public ::testing::Test
{
protected:
	fs::path db_path_;
	fs::path root_;
	std::unique_ptr<Database> db_;
	std::unique_ptr<ConfStore> conf_;
	std::unique_ptr<SyncManager> sync_;

	void SetUp() override
	{
		const auto tag = std::to_string(reinterpret_cast<uintptr_t>(this));
		root_          = fs::temp_directory_path() / ("momus_sync_" + tag);
		db_path_       = fs::temp_directory_path() / ("momus_sync_" + tag + ".sqlite");
		fs::remove_all(root_);
		fs::create_directories(root_);
		fs::remove(db_path_);

		// A real on-disk DB, not ":memory:" — SyncManager opens a *second*
		// connection to the same path_ during sync (Database::openConnection),
		// which would silently see an empty separate database if this were
		// in-memory (no shared-cache URI in play here).
		db_   = std::make_unique<Database>(db_path_.string());
		conf_ = std::make_unique<ConfStore>((root_ / "kairos.conf").string());
		sync_ = std::make_unique<SyncManager>(*db_, *conf_);
	}

	void TearDown() override
	{
		db_.reset();
		std::error_code ec;
		fs::remove_all(root_, ec);
		fs::remove(db_path_, ec);
	}

	void addSource(const std::string& source_id, int sync_priority)
	{
		SQLite::Statement s(db_->get(),
							"INSERT INTO media_source (source_id, source_type, display_name, base_url, sync_priority) "
							"VALUES (?, 'local', ?, ?, ?)");
		s.bind(1, source_id);
		s.bind(2, source_id);
		s.bind(3, (root_ / source_id).string());
		s.bind(4, sync_priority);
		s.exec();
	}

	void addLibrary(const std::string& source_id, const std::string& external_lib_id)
	{
		SQLite::Statement s(db_->get(),
							"INSERT INTO media_library (library_id, source_id, external_lib_id, display_name, library_type) "
							"VALUES (?, ?, ?, 'Movies', 'movie')");
		s.bind(1, source_id + "-lib");
		s.bind(2, source_id);
		s.bind(3, external_lib_id);
		s.exec();
	}

	// One movie folder with a movie.nfo — same title+year across sources is
	// what makes SyncManager's cross-source dedup match them onto one row.
	void writeMovie(const std::string& source_id, const std::string& folder,
					const std::string& overview, const std::string& tagline)
	{
		fs::path dir = root_ / source_id / folder;
		fs::create_directories(dir);
		std::ofstream video(dir / (folder + ".mkv"));
		video.close();

		std::ofstream nfo(dir / "movie.nfo");
		nfo << "<?xml version=\"1.0\"?><movie>"
			<< "<title>Alien</title><year>1979</year>";
		if (!overview.empty()) nfo << "<plot>" << overview << "</plot>";
		if (!tagline.empty()) nfo << "<tagline>" << tagline << "</tagline>";
		nfo << "</movie>";
		nfo.close();
	}

	struct MovieRow
	{
		std::string overview, tagline, primary_source;
		int count = 0;
	};

	MovieRow readMergedMovie()
	{
		MovieRow row;
		SQLite::Statement q(db_->get(), "SELECT overview, tagline, primary_source FROM movie WHERE title='Alien'");
		while (q.executeStep())
		{
			row.overview       = q.getColumn(0).getString();
			row.tagline        = q.getColumn(1).getString();
			row.primary_source = q.getColumn(2).getString();
			++row.count;
		}
		return row;
	}

	// A movie folder with 2 CD-marked files instead of writeMovie()'s single
	// file — exercises LocalSource's multi-part grouping (see GitHub #3)
	// through the real syncMovies()/s_upsert_movie fan-out.
	void writeMultiPartMovie(const std::string& source_id, const std::string& folder)
	{
		fs::path dir = root_ / source_id / folder;
		fs::create_directories(dir);
		std::ofstream(dir / (folder + ".CD1.mkv")).close();
		std::ofstream(dir / (folder + ".CD2.mkv")).close();

		std::ofstream nfo(dir / "movie.nfo");
		nfo << "<?xml version=\"1.0\"?><movie><title>Alien</title><year>1979</year></movie>";
		nfo.close();
	}

	struct MoviePartRow
	{
		int part_num;
		std::string file_path;
	};

	std::vector<MoviePartRow> readMovieParts()
	{
		std::vector<MoviePartRow> rows;
		SQLite::Statement q(db_->get(),
							"SELECT mp.part_num, mp.file_path FROM movie_part mp "
							"JOIN movie m ON m.movie_id = mp.movie_id "
							"WHERE m.title='Alien' ORDER BY mp.part_num");
		while (q.executeStep()) rows.push_back({q.getColumn(0).getInt(), q.getColumn(1).getString()});
		return rows;
	}

	bool readIsMultiPart()
	{
		SQLite::Statement q(db_->get(), "SELECT is_multi_part FROM movie WHERE title='Alien'");
		return q.executeStep() && q.getColumn(0).getInt() != 0;
	}
};

// plex_list_link has no FK back to playlist/filler_list (see
// PlaylistRepository::remove()'s comment on this), so a link left behind by a
// deletion path that bypasses PlaylistRepository::remove()/FillerRepository::
// remove() (e.g. POST /api/config/library/reset's raw `DELETE FROM playlist`)
// would otherwise fail syncPlexLinks()'s real FK constraint on
// playlist_item/filler_list_item every single sync, forever. syncPlexLinks()
// must self-heal: detect the missing target and delete the stale link instead
// of retrying indefinitely.
TEST_F(SyncManagerTest, SyncPlexLinks_RemovesStaleLinkWhenPlaylistNoLongerExists)
{
	addSource("src1", 1);
	sync_->loadSources();

	SQLite::Statement ins(db_->get(),
						  "INSERT INTO plex_list_link (list_type, list_id, source_id, external_id, plex_type) "
						  "VALUES ('playlist', 'ghost-playlist', 'src1', 'ext-1', 'playlist')");
	ins.exec();

	// No `playlist` row for 'ghost-playlist' exists — simulates the orphan
	// left behind by a raw-SQL delete of the playlist table.
	EXPECT_NO_THROW(sync_->syncSource("src1"));

	SQLite::Statement q(db_->get(),
						"SELECT COUNT(*) FROM plex_list_link WHERE list_id = 'ghost-playlist'");
	ASSERT_TRUE(q.executeStep());
	EXPECT_EQ(q.getColumn(0).getInt(), 0);
}

TEST_F(SyncManagerTest, SyncPlexLinks_RemovesStaleLinkWhenFillerListNoLongerExists)
{
	addSource("src1", 1);
	sync_->loadSources();

	SQLite::Statement ins(db_->get(),
						  "INSERT INTO plex_list_link (list_type, list_id, source_id, external_id, plex_type) "
						  "VALUES ('filler_list', 'ghost-filler', 'src1', 'ext-2', 'collection')");
	ins.exec();

	EXPECT_NO_THROW(sync_->syncSource("src1"));

	SQLite::Statement q(db_->get(),
						"SELECT COUNT(*) FROM plex_list_link WHERE list_id = 'ghost-filler'");
	ASSERT_TRUE(q.executeStep());
	EXPECT_EQ(q.getColumn(0).getInt(), 0);
}

// Sanity check for the same code path: a link whose target playlist DOES
// still exist must survive the self-heal check and sync normally (updating
// last_synced_at), not get treated as an orphan.
TEST_F(SyncManagerTest, SyncPlexLinks_KeepsLinkWhenPlaylistStillExists)
{
	addSource("src1", 1);
	sync_->loadSources();

	SQLite::Statement pl(db_->get(),
						 "INSERT INTO playlist (playlist_id, title, source) VALUES ('real-playlist', 'Real', 'plex')");
	pl.exec();
	SQLite::Statement ins(db_->get(),
						  "INSERT INTO plex_list_link (list_type, list_id, source_id, external_id, plex_type) "
						  "VALUES ('playlist', 'real-playlist', 'src1', 'ext-3', 'playlist')");
	ins.exec();

	EXPECT_NO_THROW(sync_->syncSource("src1"));

	SQLite::Statement q(db_->get(),
						"SELECT last_synced_at FROM plex_list_link WHERE list_id = 'real-playlist'");
	ASSERT_TRUE(q.executeStep());
	EXPECT_GT(q.getColumn(0).getInt64(), 0);
}

TEST_F(SyncManagerTest, LoadSources_OrdersByPriority)
{
	// Insert deliberately out of priority order to make sure it's the
	// ORDER BY doing the work, not insertion order coincidentally matching.
	addSource("mid", 5);
	addSource("last", 9);
	addSource("first", 1);

	sync_->loadSources();
	auto ids = sync_->sourceIds();

	ASSERT_EQ(ids.size(), 3u);
	EXPECT_EQ(ids[0], "first");
	EXPECT_EQ(ids[1], "mid");
	EXPECT_EQ(ids[2], "last");
}

TEST_F(SyncManagerTest, CrossSourceMerge_HigherPriorityWinsConflict_LowerBackfillsGap)
{
	// "hi" is the higher-priority source (lower number). Its overview should
	// win outright over "lo"'s differing overview; "lo"'s tagline (which
	// "hi" doesn't have) should backfill instead of being discarded.
	addSource("hi", 1);
	addSource("lo", 2);
	addLibrary("hi", (root_ / "hi").string());
	addLibrary("lo", (root_ / "lo").string());

	writeMovie("hi", "Alien (1979)", "The authoritative synopsis.", "");
	writeMovie("lo", "Alien (1979)", "A worse, conflicting synopsis.", "In space no one can hear you scream.");

	sync_->loadSources();
	sync_->syncAll();

	auto row = readMergedMovie();
	ASSERT_EQ(row.count, 1) << "cross-source dedup should merge onto one row, not create two";
	EXPECT_EQ(row.overview, "The authoritative synopsis.")
		<< "higher-priority source must win a genuine conflict";
	EXPECT_EQ(row.tagline, "In space no one can hear you scream.")
		<< "lower-priority source must still backfill a field the owner left empty "
		"(this is the cross_ref-never-writes-metadata bug this test guards against)";
	EXPECT_EQ(row.primary_source, "hi") << "the higher-priority source should own the merged row";
}

TEST_F(SyncManagerTest, CrossSourceMerge_ReversedSyncOrderStillConverges)
{
	// Same as above but "lo" (lower priority) happens to be configured with
	// a lower sync_priority number... no — deliberately sync "lo" via a
	// second pass after "hi" already lost a prior race, to prove the result
	// is priority-rank-based (primary_source-gated), not just "whichever
	// source's data happened to land in the row first".
	addSource("hi", 1);
	addSource("lo", 2);
	addLibrary("hi", (root_ / "hi").string());
	addLibrary("lo", (root_ / "lo").string());

	writeMovie("lo", "Alien (1979)", "Low-priority synopsis.", "Low-priority tagline.");
	writeMovie("hi", "Alien (1979)", "High-priority synopsis.", "");

	sync_->loadSources();
	// Sync the lower-priority source FIRST on its own, then the higher-
	// priority one — loadSources() already orders sources_ correctly, but
	// this test calls syncSource() directly per source to prove the merge
	// still lands correctly even against an adversarial call order.
	sync_->syncSource("lo");
	sync_->syncSource("hi");

	auto row = readMergedMovie();
	ASSERT_EQ(row.count, 1);
	EXPECT_EQ(row.overview, "High-priority synopsis.")
		<< "hi must reclaim/overwrite lo's conflicting value even though lo synced first";
	EXPECT_EQ(row.tagline, "Low-priority tagline.")
		<< "lo's tagline should survive as a backfill since hi never provided one";
	EXPECT_EQ(row.primary_source, "hi");
}

TEST_F(SyncManagerTest, CrossSourceMerge_ConfirmedMatchBlocksReclaim)
{
	// "lo" claims the item first (its own scraper match then gets confirmed
	// by a human — simulated directly here rather than driving the real
	// scraper queue, which is a separate subsystem). A subsequently-synced
	// higher-priority "hi" must not be able to overwrite lo's already-set,
	// human-confirmed overview just by outranking it — only a manual field
	// edit (locked=1) is supposed to grant that kind of absolute override
	// immunity; match_confirmed alone should merely freeze *ownership*, not
	// block genuine gap-filling.
	addSource("hi", 1);
	addSource("lo", 2);
	addLibrary("hi", (root_ / "hi").string());
	addLibrary("lo", (root_ / "lo").string());

	writeMovie("lo", "Alien (1979)", "Low-priority synopsis.", "");
	writeMovie("hi", "Alien (1979)", "High-priority synopsis.", "High-priority tagline.");

	sync_->loadSources();
	sync_->syncSource("lo");

	{
		SQLite::Statement s(db_->get(), "UPDATE movie SET match_confirmed = 1 WHERE title='Alien'");
		s.exec();
	}

	sync_->syncSource("hi");

	auto row = readMergedMovie();
	ASSERT_EQ(row.count, 1);
	EXPECT_EQ(row.overview, "Low-priority synopsis.")
		<< "a confirmed match must not be reclaimed/overwritten by a higher-priority source";
	EXPECT_EQ(row.tagline, "High-priority tagline.")
		<< "backfilling a genuine gap must still work even on a confirmed item";
	EXPECT_EQ(row.primary_source, "lo") << "ownership must not change once confirmed";
}

// Multi-part movies (GitHub #3): a movie folder with CD1/CD2-marked files
// must land as one movie row with is_multi_part=1 and a movie_part row per
// file, through the real LocalSource::fetchMovies -> SyncManager::syncMovies
// -> s_upsert_movie fan-out — not as duplicate/orphan movie rows.
TEST_F(SyncManagerTest, MultiPartMovie_FansOutIntoMoviePartRows)
{
	addSource("src1", 1);
	addLibrary("src1", (root_ / "src1").string());
	writeMultiPartMovie("src1", "Alien (1979)");

	sync_->loadSources();
	sync_->syncAll();

	auto row = readMergedMovie();
	ASSERT_EQ(row.count, 1) << "multi-part files must merge onto one movie row, not create duplicates";
	EXPECT_TRUE(readIsMultiPart());

	auto parts = readMovieParts();
	ASSERT_EQ(parts.size(), 2u);
	EXPECT_EQ(parts[0].part_num, 1);
	EXPECT_EQ(parts[1].part_num, 2);
	EXPECT_EQ(parts[0].file_path, (root_ / "src1" / "Alien (1979)" / "Alien (1979).CD1.mkv").string());
	EXPECT_EQ(parts[1].file_path, (root_ / "src1" / "Alien (1979)" / "Alien (1979).CD2.mkv").string());
}

// Re-syncing (e.g. a part renamed/removed) must not leave stale movie_part
// rows behind — s_delete_parts/s_insert_part's delete-then-reinsert should
// converge movie_part to exactly what the current fetch reports.
TEST_F(SyncManagerTest, MultiPartMovie_ResyncConvergesPartsToCurrentState)
{
	addSource("src1", 1);
	addLibrary("src1", (root_ / "src1").string());
	writeMultiPartMovie("src1", "Alien (1979)");

	sync_->loadSources();
	sync_->syncAll();
	ASSERT_EQ(readMovieParts().size(), 2u);

	// Drop CD2 — now looks like a single-file movie on disk.
	fs::remove(root_ / "src1" / "Alien (1979)" / "Alien (1979).CD2.mkv");
	sync_->syncAll();

	EXPECT_FALSE(readIsMultiPart());
	EXPECT_TRUE(readMovieParts().empty());
}

// A manually-linked multi-part movie (ContentRepository::linkMovieParts sets
// movie.locked=1 and writes origin='manual' movie_part rows) must survive a
// resync of the target's own source untouched — otherwise the source's own
// single-file reality would silently overwrite the admin's manual grouping
// every sync, exactly the bug this locked/origin scoping exists to prevent.
TEST_F(SyncManagerTest, MultiPartMovie_ManuallyLinkedSurvivesResync)
{
	addSource("src1", 1);
	addLibrary("src1", (root_ / "src1").string());
	writeMovie("src1", "Alien (1979)", "", "");

	sync_->loadSources();
	sync_->syncAll();

	std::string movie_id;
	{
		SQLite::Statement q(db_->get(), "SELECT movie_id FROM movie WHERE title='Alien'");
		ASSERT_TRUE(q.executeStep());
		movie_id = q.getColumn(0).getString();
	}

	// Simulate ContentRepository::linkMovieParts having absorbed a second,
	// unrelated file into this movie as manual part 2.
	{
		SQLite::Statement upd(db_->get(),
							  "UPDATE movie SET is_multi_part = 1, duration_ms = 999999, locked = 1 WHERE movie_id = ?");
		upd.bind(1, movie_id);
		upd.exec();
	}
	{
		SQLite::Statement ins(db_->get(), R"(
            INSERT INTO movie_part (movie_id, part_num, file_path, duration_ms, origin)
            VALUES (?,1,?,500000,'manual'), (?,2,'/elsewhere/part2.mkv',499999,'manual')
        )");
		ins.bind(1, movie_id);
		ins.bind(2, (root_ / "src1" / "Alien (1979)" / "Alien (1979).mkv").string());
		ins.bind(3, movie_id);
		ins.exec();
	}

	sync_->syncAll();

	SQLite::Statement q(db_->get(), "SELECT is_multi_part, locked, duration_ms FROM movie WHERE movie_id = ?");
	q.bind(1, movie_id);
	ASSERT_TRUE(q.executeStep());
	EXPECT_EQ(q.getColumn(0).getInt(), 1);
	EXPECT_EQ(q.getColumn(1).getInt(), 1);
	EXPECT_EQ(q.getColumn(2).getInt64(), 999999) << "locked movie's admin-set duration must not be rederived from the single on-disk file";

	SQLite::Statement pq(db_->get(), "SELECT COUNT(*) FROM movie_part WHERE movie_id = ? AND origin = 'manual'");
	pq.bind(1, movie_id);
	ASSERT_TRUE(pq.executeStep());
	EXPECT_EQ(pq.getColumn(0).getInt(), 2) << "manual parts must survive a resync of the target's own source";
}