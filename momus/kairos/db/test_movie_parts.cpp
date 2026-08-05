// Tests for ContentRepository::linkMovieParts/unlinkMoviePart/
// getMovieGroupingCandidates — the manual multi-part movie admin flow
// (GitHub #3). linkMovieParts reuses mergeMovieIntoNoTxn's FK cleanup, so
// this focuses on the movie_part/locked/is_multi_part bookkeeping that
// layer adds rather than re-testing the merge FK cleanup itself (see
// db/test_content_repository_writeback.cpp and api/test_writeback_all.cpp
// for adjacent ContentRepository coverage).

#include <gtest/gtest.h>
#include "db/ContentRepository.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <string>
#include <vector>

class MoviePartsTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ContentRepository repo{db};

	void insertMovie(const std::string& id, const std::string& file_path, int64_t duration_ms)
	{
		SQLite::Statement s(db.get(),
							"INSERT INTO movie (movie_id, title, file_path, duration_ms) VALUES (?,?,?,?)");
		s.bind(1, id);
		s.bind(2, id + " title");
		s.bind(3, file_path);
		s.bind(4, duration_ms);
		s.exec();
	}

	struct PartRow
	{
		int part_num;
		std::string file_path;
		int64_t duration_ms;
		std::string origin;
	};

	std::vector<PartRow> readParts(const std::string& movie_id)
	{
		std::vector<PartRow> out;
		SQLite::Statement q(db.get(),
							"SELECT part_num, file_path, duration_ms, origin FROM movie_part WHERE movie_id = ? ORDER BY part_num");
		q.bind(1, movie_id);
		while (q.executeStep())
			out.push_back({
				q.getColumn(0).getInt(), q.getColumn(1).getString(),
				q.getColumn(2).getInt64(), q.getColumn(3).getString()
			});
		return out;
	}

	struct MovieState
	{
		bool exists         = false;
		bool is_multi_part  = false;
		bool locked         = false;
		int64_t duration_ms = 0;
		std::string file_path;
	};

	MovieState readMovie(const std::string& movie_id)
	{
		MovieState st;
		SQLite::Statement q(db.get(), "SELECT is_multi_part, locked, duration_ms, file_path FROM movie WHERE movie_id = ?");
		q.bind(1, movie_id);
		if (q.executeStep())
		{
			st.exists        = true;
			st.is_multi_part = q.getColumn(0).getInt() != 0;
			st.locked        = q.getColumn(1).getInt() != 0;
			st.duration_ms   = q.getColumn(2).getInt64();
			st.file_path     = q.getColumn(3).getString();
		}
		return st;
	}
};

// ---------------------------------------------------------------------------
// linkMovieParts
// ---------------------------------------------------------------------------

TEST_F(MoviePartsTest, Link_CreatesOrderedPartsAndSumsDuration)
{
	insertMovie("m1", "/media/m1.cd1.mkv", 3000);
	insertMovie("m2", "/media/m1.cd2.mkv", 3200);

	repo.linkMovieParts("m1", {"m2"});

	auto parts = readParts("m1");
	ASSERT_EQ(parts.size(), 2u);
	EXPECT_EQ(parts[0].part_num, 1);
	EXPECT_EQ(parts[0].file_path, "/media/m1.cd1.mkv");
	EXPECT_EQ(parts[0].origin, "manual");
	EXPECT_EQ(parts[1].part_num, 2);
	EXPECT_EQ(parts[1].file_path, "/media/m1.cd2.mkv");

	auto target = readMovie("m1");
	EXPECT_TRUE(target.is_multi_part);
	EXPECT_TRUE(target.locked);
	EXPECT_EQ(target.duration_ms, 6200);

	EXPECT_FALSE(readMovie("m2").exists) << "absorbed movie row must be deleted, same as a plain merge";
}

TEST_F(MoviePartsTest, Link_ThreeParts_PreservesGivenOrder)
{
	insertMovie("m1", "/media/m1.cd1.mkv", 1000);
	insertMovie("m2", "/media/m1.cd2.mkv", 1000);
	insertMovie("m3", "/media/m1.cd3.mkv", 1000);

	repo.linkMovieParts("m1", {"m2", "m3"});

	auto parts = readParts("m1");
	ASSERT_EQ(parts.size(), 3u);
	EXPECT_EQ(parts[0].file_path, "/media/m1.cd1.mkv");
	EXPECT_EQ(parts[1].file_path, "/media/m1.cd2.mkv");
	EXPECT_EQ(parts[2].file_path, "/media/m1.cd3.mkv");
	EXPECT_EQ(readMovie("m1").duration_ms, 3000);
}

TEST_F(MoviePartsTest, Link_ThrowsOnSelfLink)
{
	insertMovie("m1", "/media/m1.mkv", 1000);
	EXPECT_THROW(repo.linkMovieParts("m1", {"m1"}), std::runtime_error);
}

TEST_F(MoviePartsTest, Link_ThrowsOnEmptyList)
{
	insertMovie("m1", "/media/m1.mkv", 1000);
	EXPECT_THROW(repo.linkMovieParts("m1", {}), std::runtime_error);
}

TEST_F(MoviePartsTest, Link_ThrowsOnMissingMovie)
{
	insertMovie("m1", "/media/m1.mkv", 1000);
	EXPECT_THROW(repo.linkMovieParts("m1", {"ghost"}), std::runtime_error);
}

// ---------------------------------------------------------------------------
// unlinkMoviePart
// ---------------------------------------------------------------------------

TEST_F(MoviePartsTest, Unlink_SplitsPartIntoNewStandaloneMovie)
{
	insertMovie("m1", "/media/m1.cd1.mkv", 3000);
	insertMovie("m2", "/media/m1.cd2.mkv", 3200);
	repo.linkMovieParts("m1", {"m2"});

	repo.unlinkMoviePart("m1", 2);

	auto target = readMovie("m1");
	EXPECT_FALSE(target.is_multi_part) << "only 1 part remains — collapses back to single-file";
	EXPECT_EQ(target.duration_ms, 3000);
	EXPECT_EQ(target.file_path, "/media/m1.cd1.mkv");
	EXPECT_TRUE(readParts("m1").empty());

	// The split-off part now lives as its own standalone movie row.
	SQLite::Statement q(db.get(), "SELECT file_path, duration_ms FROM movie WHERE file_path = '/media/m1.cd2.mkv'");
	ASSERT_TRUE(q.executeStep());
	EXPECT_EQ(q.getColumn(1).getInt64(), 3200);
}

TEST_F(MoviePartsTest, Unlink_ThreeParts_RenumbersRemainingParts)
{
	insertMovie("m1", "/media/m1.cd1.mkv", 1000);
	insertMovie("m2", "/media/m1.cd2.mkv", 1000);
	insertMovie("m3", "/media/m1.cd3.mkv", 1000);
	repo.linkMovieParts("m1", {"m2", "m3"});

	repo.unlinkMoviePart("m1", 2); // splits off the middle part

	auto parts = readParts("m1");
	ASSERT_EQ(parts.size(), 2u);
	EXPECT_EQ(parts[0].part_num, 1);
	EXPECT_EQ(parts[0].file_path, "/media/m1.cd1.mkv");
	EXPECT_EQ(parts[1].part_num, 2) << "part 3 must be renumbered down to fill the gap left by removing part 2";
	EXPECT_EQ(parts[1].file_path, "/media/m1.cd3.mkv");

	auto target = readMovie("m1");
	EXPECT_TRUE(target.is_multi_part);
	EXPECT_EQ(target.duration_ms, 2000);
}

TEST_F(MoviePartsTest, Unlink_ThrowsWhenMovieHasNoParts)
{
	insertMovie("m1", "/media/m1.mkv", 1000);
	EXPECT_THROW(repo.unlinkMoviePart("m1", 1), std::runtime_error);
}

TEST_F(MoviePartsTest, Unlink_ThrowsWhenPartNumNotFound)
{
	insertMovie("m1", "/media/m1.cd1.mkv", 1000);
	insertMovie("m2", "/media/m1.cd2.mkv", 1000);
	repo.linkMovieParts("m1", {"m2"});
	EXPECT_THROW(repo.unlinkMoviePart("m1", 99), std::runtime_error);
}

// ---------------------------------------------------------------------------
// getMovieGroupingCandidates
// ---------------------------------------------------------------------------

TEST_F(MoviePartsTest, Candidates_DetectsConsistentPair)
{
	insertMovie("m1", "/media/Movie Title CD1.mkv", 1000);
	insertMovie("m2", "/media/Movie Title CD2.mkv", 1000);

	auto candidates = repo.getMovieGroupingCandidates();
	ASSERT_EQ(candidates.size(), 1u);
	EXPECT_EQ(candidates[0].parts.size(), 2u);
	EXPECT_GE(candidates[0].confidence, 40);
}

TEST_F(MoviePartsTest, Candidates_ExcludesAlreadyMultiPartMovies)
{
	insertMovie("m1", "/media/m1.cd1.mkv", 1000);
	insertMovie("m2", "/media/m1.cd2.mkv", 1000);
	repo.linkMovieParts("m1", {"m2"});

	// Two more standalone CD-marked files elsewhere must still surface...
	insertMovie("m3", "/media/Other Movie CD1.mkv", 1000);
	insertMovie("m4", "/media/Other Movie CD2.mkv", 1000);

	auto candidates = repo.getMovieGroupingCandidates();
	ASSERT_EQ(candidates.size(), 1u) << "the already-linked movie must not reappear as a candidate";
	EXPECT_EQ(candidates[0].parts.front().movie_id, "m3");
}

TEST_F(MoviePartsTest, Candidates_EmptyWhenNoMarkersPresent)
{
	insertMovie("m1", "/media/Alien.mkv", 1000);
	insertMovie("m2", "/media/Predator.mkv", 1000);
	EXPECT_TRUE(repo.getMovieGroupingCandidates().empty());
}