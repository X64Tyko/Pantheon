// Tests for LocalSource filesystem parsing.
// Uses real temporary directories to exercise: directory enumeration, hidden
// entry skipping, title/year parsing, season dir recognition, episode filename
// regex (S01E01 and 1x01 forms), movie layout variants, and the path-traversal
// guard in listSubdirectories.
//
// No HTTP server is involved — LocalSource is filesystem-only.

#include <gtest/gtest.h>
#include "source/LocalSource.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "conf/ConfStore.h"

namespace fs = std::filesystem;

// ============================================================================
// Fixture — creates a unique temp root per test, cleans up in TearDown
// ============================================================================

class LocalSourceTest : public ::testing::Test
{
protected:
	fs::path root_;
	std::unique_ptr<LocalSource> src_;
	std::unique_ptr<ConfStore> conf_ = std::make_unique<ConfStore>("./kairos.conf");

	void SetUp() override
	{
		root_ = fs::temp_directory_path() /
			("momus_local_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
		fs::remove_all(root_);
		fs::create_directories(root_);
		src_ = std::make_unique<LocalSource>("src", root_.string(), *conf_);
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(root_, ec);
	}

	// Create an empty file at path p, making parent dirs as needed.
	void touch(const fs::path& p) const
	{
		fs::create_directories(p.parent_path());
		std::ofstream{p}.close();
	}
};

// ============================================================================
// listAvailableLibraries
// ============================================================================

TEST_F(LocalSourceTest, ListLibraries_SkipsHiddenDirs)
{
	fs::create_directories(root_ / ".hidden");
	fs::create_directories(root_ / "Movies");
	fs::create_directories(root_ / "TV Shows");
	const auto libs = src_->listAvailableLibraries();
	ASSERT_EQ(libs.size(), 2u);
	// Names are sorted, so: Movies, TV Shows
	EXPECT_EQ(libs[0].name, "Movies");
	EXPECT_EQ(libs[1].name, "TV Shows");
}

TEST_F(LocalSourceTest, ListLibraries_SortedAlphabetically)
{
	fs::create_directories(root_ / "Z Library");
	fs::create_directories(root_ / "A Library");
	const auto libs = src_->listAvailableLibraries();
	ASSERT_EQ(libs.size(), 2u);
	EXPECT_EQ(libs[0].name, "A Library");
	EXPECT_EQ(libs[1].name, "Z Library");
}

TEST_F(LocalSourceTest, ListLibraries_FallsBackToRootIfNoSubdirs)
{
	// root_ has no subdirectories
	const auto libs = src_->listAvailableLibraries();
	ASSERT_EQ(libs.size(), 1u);
	EXPECT_EQ(libs[0].external_lib_id, root_.string());
	EXPECT_EQ(libs[0].name, root_.filename().string());
	EXPECT_EQ(libs[0].type, "mixed");
}

TEST_F(LocalSourceTest, ListLibraries_ExternalIdIsFullPath)
{
	fs::create_directories(root_ / "Movies");
	const auto libs = src_->listAvailableLibraries();
	ASSERT_EQ(libs.size(), 1u);
	EXPECT_EQ(libs[0].external_lib_id, (root_ / "Movies").string());
}

TEST_F(LocalSourceTest, ListLibraries_GuessesShowType)
{
	// grandchild season dir triggers "show" detection
	fs::create_directories(root_ / "TV" / "Breaking Bad" / "Season 01");
	const auto libs = src_->listAvailableLibraries();
	ASSERT_EQ(libs.size(), 1u);
	EXPECT_EQ(libs[0].type, "show");
}

TEST_F(LocalSourceTest, ListLibraries_GuessesMovieType)
{
	// child dir with video files triggers "movie" detection
	touch(root_ / "Movies" / "Inception" / "Inception.mkv");
	const auto libs = src_->listAvailableLibraries();
	ASSERT_EQ(libs.size(), 1u);
	EXPECT_EQ(libs[0].type, "movie");
}

TEST_F(LocalSourceTest, ListLibraries_GuessesMixedForNoContent)
{
	fs::create_directories(root_ / "Empty Lib" / "subfolder");
	const auto libs = src_->listAvailableLibraries();
	ASSERT_EQ(libs.size(), 1u);
	EXPECT_EQ(libs[0].type, "mixed");
}

// ============================================================================
// listSubdirectories — path-traversal guard
// ============================================================================

TEST_F(LocalSourceTest, ListSubdirectories_RejectsPathThatEscapesBase)
{
	// root_.parent_path() is a sibling or ancestor — must be rejected
	const auto result = src_->listSubdirectories(root_.parent_path().string());
	EXPECT_TRUE(result.empty());
}

TEST_F(LocalSourceTest, ListSubdirectories_ListsNonHiddenSortedSubdirs)
{
	fs::create_directories(root_ / "shows" / "Breaking Bad");
	fs::create_directories(root_ / "shows" / "Better Call Saul");
	fs::create_directories(root_ / "shows" / ".hidden");
	const auto result = src_->listSubdirectories((root_ / "shows").string());
	ASSERT_EQ(result.size(), 2u);
	EXPECT_EQ(result[0].name, "Better Call Saul");
	EXPECT_EQ(result[1].name, "Breaking Bad");
}

// ============================================================================
// fetchShows
// ============================================================================

TEST_F(LocalSourceTest, FetchShows_ParsesTitleAndYear)
{
	fs::create_directories(root_ / "tv" / "Breaking Bad (2008)" / "Season 01");
	const auto shows = src_->fetchShows((root_ / "tv").string());
	ASSERT_EQ(shows.size(), 1u);
	EXPECT_EQ(shows[0].title, "Breaking Bad");
	ASSERT_TRUE(shows[0].year.has_value());
	EXPECT_EQ(*shows[0].year, 2008);
}

TEST_F(LocalSourceTest, FetchShows_TitleWithoutYear)
{
	fs::create_directories(root_ / "tv" / "Anime Show" / "Season 01");
	const auto shows = src_->fetchShows((root_ / "tv").string());
	ASSERT_EQ(shows.size(), 1u);
	EXPECT_EQ(shows[0].title, "Anime Show");
	EXPECT_FALSE(shows[0].year.has_value());
}

TEST_F(LocalSourceTest, FetchShows_StripsCompleteSeasonSuffix)
{
	fs::create_directories(root_ / "tv" / "Show Name Complete Season 1" / "Season 01");
	const auto shows = src_->fetchShows((root_ / "tv").string());
	ASSERT_EQ(shows.size(), 1u);
	EXPECT_EQ(shows[0].title, "Show Name");
}

TEST_F(LocalSourceTest, FetchShows_StripsDottedCompleteSeasonsRangeSuffix)
{
	fs::create_directories(root_ / "tv" / "Show.Name.Complete.Seasons.1-3" / "Season 01");
	const auto shows = src_->fetchShows((root_ / "tv").string());
	ASSERT_EQ(shows.size(), 1u);
	EXPECT_EQ(shows[0].title, "Show Name");
}

// A collection/season descriptor that sits *before* the year in the folder
// name still has to win the title cut, even though the year is found too —
// regression test for the case where the year unconditionally won the cut
// regardless of what preceded it.
TEST_F(LocalSourceTest, FetchShows_StripsTvSeriesSuffixBeforeYear)
{
	fs::create_directories(root_ / "tv" / "Batman TV Series 1966" / "Season 01");
	const auto shows = src_->fetchShows((root_ / "tv").string());
	ASSERT_EQ(shows.size(), 1u);
	EXPECT_EQ(shows[0].title, "Batman");
	ASSERT_TRUE(shows[0].year.has_value());
	EXPECT_EQ(*shows[0].year, 1966);
}

TEST_F(LocalSourceTest, FetchShows_SkipsHiddenDirs)
{
	fs::create_directories(root_ / "tv" / ".hidden_show" / "Season 01");
	fs::create_directories(root_ / "tv" / "Normal Show" / "Season 01");
	const auto shows = src_->fetchShows((root_ / "tv").string());
	ASSERT_EQ(shows.size(), 1u);
	EXPECT_EQ(shows[0].title, "Normal Show");
}

TEST_F(LocalSourceTest, FetchShows_SortedByTitle)
{
	fs::create_directories(root_ / "tv" / "Zorro (1990)" / "Season 01");
	fs::create_directories(root_ / "tv" / "Archer (2009)" / "Season 01");
	const auto shows = src_->fetchShows((root_ / "tv").string());
	ASSERT_EQ(shows.size(), 2u);
	EXPECT_EQ(shows[0].title, "Archer");
	EXPECT_EQ(shows[1].title, "Zorro");
}

TEST_F(LocalSourceTest, FetchShows_ShowIdIsFullPath)
{
	fs::create_directories(root_ / "tv" / "Firefly (2002)" / "Season 01");
	const auto shows = src_->fetchShows((root_ / "tv").string());
	ASSERT_EQ(shows.size(), 1u);
	EXPECT_EQ(shows[0].show_id, (root_ / "tv" / "Firefly (2002)").string());
}

TEST_F(LocalSourceTest, FetchShows_EmptyForNonexistentPath)
{
	EXPECT_TRUE(src_->fetchShows("/nonexistent/path/does/not/exist").empty());
}

// ============================================================================
// fetchMovies
// ============================================================================

TEST_F(LocalSourceTest, FetchMovies_FolderLayout)
{
	touch(root_ / "movies" / "The Matrix (1999)" / "The.Matrix.1999.mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);
	EXPECT_EQ(movies[0].title, "The Matrix");
	ASSERT_TRUE(movies[0].year.has_value());
	EXPECT_EQ(*movies[0].year, 1999);
	EXPECT_EQ(movies[0].file_path, (root_ / "movies" / "The Matrix (1999)" / "The.Matrix.1999.mkv").string());
}

TEST_F(LocalSourceTest, FetchMovies_BareFileLayout)
{
	touch(root_ / "movies" / "Inception (2010).mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);
	EXPECT_EQ(movies[0].title, "Inception");
	ASSERT_TRUE(movies[0].year.has_value());
	EXPECT_EQ(*movies[0].year, 2010);
	EXPECT_EQ(movies[0].file_path, (root_ / "movies" / "Inception (2010).mkv").string());
}

TEST_F(LocalSourceTest, FetchMovies_SkipsEmptyFolders)
{
	touch(root_ / "movies" / "Empty Folder" / "readme.txt"); // not a video
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	EXPECT_TRUE(movies.empty());
}

TEST_F(LocalSourceTest, FetchMovies_SkipsHiddenFiles)
{
	touch(root_ / "movies" / ".hidden.mkv");
	touch(root_ / "movies" / "Real Movie.mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);
	EXPECT_EQ(movies[0].title, "Real Movie");
}

TEST_F(LocalSourceTest, FetchMovies_SortedByTitle)
{
	touch(root_ / "movies" / "Zodiac.mkv");
	touch(root_ / "movies" / "Alien.mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 2u);
	EXPECT_EQ(movies[0].title, "Alien");
	EXPECT_EQ(movies[1].title, "Zodiac");
}

TEST_F(LocalSourceTest, FetchMovies_MixedLayouts)
{
	touch(root_ / "movies" / "Interstellar (2014)" / "Interstellar.mkv"); // folder layout
	touch(root_ / "movies" / "Dune (2021).mp4");                          // bare file
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 2u);
	// sorted: Dune, Interstellar
	EXPECT_EQ(movies[0].title, "Dune");
	EXPECT_EQ(movies[1].title, "Interstellar");
}

// ============================================================================
// fetchMovies — multi-part grouping (GitHub #3)
// ============================================================================

TEST_F(LocalSourceTest, FetchMovies_FolderLayout_MultiPartGrouped)
{
	touch(root_ / "movies" / "Movie Title (2020)" / "Movie.Title.CD1.mkv");
	touch(root_ / "movies" / "Movie Title (2020)" / "Movie.Title.CD2.mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);
	EXPECT_TRUE(movies[0].is_multi_part);
	ASSERT_EQ(movies[0].parts.size(), 2u);
	EXPECT_EQ(movies[0].parts[0].part_num, 1);
	EXPECT_EQ(movies[0].parts[1].part_num, 2);
	EXPECT_EQ(movies[0].parts[0].file_path, (root_ / "movies" / "Movie Title (2020)" / "Movie.Title.CD1.mkv").string());
	EXPECT_EQ(movies[0].parts[1].file_path, (root_ / "movies" / "Movie Title (2020)" / "Movie.Title.CD2.mkv").string());
	EXPECT_EQ(movies[0].file_path, movies[0].parts[0].file_path);
}

TEST_F(LocalSourceTest, FetchMovies_FolderLayout_SingleFileNotMultiPart)
{
	touch(root_ / "movies" / "The Matrix (1999)" / "The.Matrix.1999.mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);
	EXPECT_FALSE(movies[0].is_multi_part);
	EXPECT_TRUE(movies[0].parts.empty());
}

TEST_F(LocalSourceTest, FetchMovies_FolderLayout_InconsistentFilesNotGrouped)
{
	// A trailer alongside the main feature carries no part marker at all —
	// must not be swept into a bogus 2-part movie.
	touch(root_ / "movies" / "Movie Title (2020)" / "Movie.Title.CD1.mkv");
	touch(root_ / "movies" / "Movie Title (2020)" / "trailer.mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);
	EXPECT_FALSE(movies[0].is_multi_part);
	EXPECT_TRUE(movies[0].parts.empty());
}

TEST_F(LocalSourceTest, FetchMovies_BareFileLayout_MultiPartGrouped)
{
	touch(root_ / "movies" / "Movie Title (2020) CD1.mkv");
	touch(root_ / "movies" / "Movie Title (2020) CD2.mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);
	EXPECT_EQ(movies[0].title, "Movie Title");
	ASSERT_TRUE(movies[0].year.has_value());
	EXPECT_EQ(*movies[0].year, 2020);
	EXPECT_TRUE(movies[0].is_multi_part);
	ASSERT_EQ(movies[0].parts.size(), 2u);
	EXPECT_EQ(movies[0].parts[0].part_num, 1);
	EXPECT_EQ(movies[0].parts[1].part_num, 2);
}

TEST_F(LocalSourceTest, FetchMovies_BareFileLayout_UnrelatedFilesStayIndependent)
{
	touch(root_ / "movies" / "Alien.mkv");
	touch(root_ / "movies" / "Zodiac.mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 2u);
	EXPECT_FALSE(movies[0].is_multi_part);
	EXPECT_FALSE(movies[1].is_multi_part);
}

TEST_F(LocalSourceTest, FetchMovies_BareFileLayout_DuplicatePartNumbersNotGrouped)
{
	// Two files with the exact same base title both claiming "Part 1" is
	// more likely a naming collision than a real multi-part movie.
	touch(root_ / "movies" / "Movie Title Part 1.mkv");
	touch(root_ / "movies" / "Movie Title Part 1.mp4");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 2u);
	EXPECT_FALSE(movies[0].is_multi_part);
	EXPECT_FALSE(movies[1].is_multi_part);
}

// ============================================================================
// fetchEpisodes
// ============================================================================

TEST_F(LocalSourceTest, FetchEpisodes_SeasonDirLayout)
{
	touch(root_ / "Season 01" / "S01E01 - Pilot.mkv");
	touch(root_ / "Season 01" / "S01E02 - Bag.mkv");
	touch(root_ / "Season 02" / "S02E01 - Seven.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 3u);
	// sorted by (season, episode)
	EXPECT_EQ(eps[0].season, 1);
	EXPECT_EQ(eps[0].episode, 1);
	EXPECT_EQ(eps[0].title, "Pilot");
	EXPECT_EQ(eps[1].season, 1);
	EXPECT_EQ(eps[1].episode, 2);
	EXPECT_EQ(eps[1].title, "Bag");
	EXPECT_EQ(eps[2].season, 2);
	EXPECT_EQ(eps[2].episode, 1);
	EXPECT_EQ(eps[2].title, "Seven");
}

TEST_F(LocalSourceTest, FetchEpisodes_FlatLayout)
{
	touch(root_ / "S01E01 - Pilot.mkv");
	touch(root_ / "S02E05 - Episode.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 2u);
	EXPECT_EQ(eps[0].season, 1);
	EXPECT_EQ(eps[0].episode, 1);
	EXPECT_EQ(eps[1].season, 2);
	EXPECT_EQ(eps[1].episode, 5);
}

TEST_F(LocalSourceTest, FetchEpisodes_AlternateNotation_1x01)
{
	touch(root_ / "Season 01" / "1x05 - Five.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 1u);
	EXPECT_EQ(eps[0].season, 1);
	EXPECT_EQ(eps[0].episode, 5);
	EXPECT_EQ(eps[0].title, "Five");
}

TEST_F(LocalSourceTest, FetchEpisodes_AlternateNotation_1X01_Uppercase)
{
	touch(root_ / "Season 02" / "2X03 - Three.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 1u);
	EXPECT_EQ(eps[0].season, 2);
	EXPECT_EQ(eps[0].episode, 3);
}

TEST_F(LocalSourceTest, FetchEpisodes_MultiEpisode_ConcatenatedForm)
{
	// "S01E01E02" — two E-groups back to back, no separator.
	touch(root_ / "Season 01" / "S01E01E02 - Double.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 2u);
	EXPECT_EQ(eps[0].season, 1);
	EXPECT_EQ(eps[0].episode, 1);
	EXPECT_EQ(eps[1].season, 1);
	EXPECT_EQ(eps[1].episode, 2);
	// Both rows point at the same physical file.
	EXPECT_EQ(eps[0].file_path, eps[1].file_path);
	// But get distinct primary keys so they don't collide in the DB.
	EXPECT_NE(eps[0].episode_id, eps[1].episode_id);
}

TEST_F(LocalSourceTest, FetchEpisodes_MultiEpisode_DashEForm)
{
	// "S01E01-E02" — dash then a second E-group.
	touch(root_ / "S01E01-E02 - Double.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 2u);
	EXPECT_EQ(eps[0].episode, 1);
	EXPECT_EQ(eps[1].episode, 2);
}

TEST_F(LocalSourceTest, FetchEpisodes_MultiEpisode_DashBareNumberForm)
{
	// "S01E01-02" — dash then a bare number, no repeated "E".
	touch(root_ / "S01E01-02 - Double.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 2u);
	EXPECT_EQ(eps[0].episode, 1);
	EXPECT_EQ(eps[1].episode, 2);
}

TEST_F(LocalSourceTest, FetchEpisodes_MultiEpisode_ThreePartArc)
{
	touch(root_ / "S01E01-E03 - Arc.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 3u);
	EXPECT_EQ(eps[0].episode, 1);
	EXPECT_EQ(eps[1].episode, 2);
	EXPECT_EQ(eps[2].episode, 3);
}

TEST_F(LocalSourceTest, FetchEpisodes_MultiEpisode_AltNotationRange)
{
	touch(root_ / "Season 01" / "1x05-06 - Double.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 2u);
	EXPECT_EQ(eps[0].episode, 5);
	EXPECT_EQ(eps[1].episode, 6);
}

TEST_F(LocalSourceTest, FetchEpisodes_DashFollowedByQualityTagIsNotMisreadAsRange)
{
	// "S01E01-1080p..." must not be parsed as a range ending at episode 108 —
	// the digits belong to an adjacent quality tag, not a second episode number.
	touch(root_ / "Show.Name.S01E01-1080p.WEB-DL.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 1u);
	EXPECT_EQ(eps[0].season, 1);
	EXPECT_EQ(eps[0].episode, 1);
}

TEST_F(LocalSourceTest, FetchEpisodes_ImplausiblyWideRangeFallsBackToSingleEpisode)
{
	// A "range" spanning far more than a real multi-part episode file would
	// is treated as a bad match, not exploded into dozens of episode rows.
	touch(root_ / "Show.Name.S01E01E99.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 1u);
	EXPECT_EQ(eps[0].episode, 1);
}

TEST_F(LocalSourceTest, FetchEpisodes_SingleEpisodeIdUnaffectedByMultiEpisodeSupport)
{
	// Regression guard for FetchEpisodes_EpisodeIdIsFullPath: single-episode
	// files must keep episode_id == file path exactly, no suffix added.
	touch(root_ / "S01E01 - Pilot.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 1u);
	EXPECT_EQ(eps[0].episode_id, (root_ / "S01E01 - Pilot.mkv").string());
}

TEST_F(LocalSourceTest, FetchEpisodes_UnparseableFilenameGetsStemAsTitle)
{
	touch(root_ / "RandomFile.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 1u);
	EXPECT_EQ(eps[0].season, 1);
	EXPECT_EQ(eps[0].episode, 0);
	EXPECT_EQ(eps[0].title, "RandomFile");
}

TEST_F(LocalSourceTest, FetchEpisodes_SeasonDirParsing_SPrefix)
{
	// "S01" shorthand should be recognised as season 1
	touch(root_ / "S01" / "S01E01 - First.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 1u);
	EXPECT_EQ(eps[0].season, 1);
}

TEST_F(LocalSourceTest, FetchEpisodes_SeasonDirParsing_SeriesPrefix)
{
	touch(root_ / "Series 3" / "S03E01 - Start.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 1u);
	EXPECT_EQ(eps[0].season, 3);
}

TEST_F(LocalSourceTest, FetchEpisodes_EpisodeIdIsFullPath)
{
	touch(root_ / "S01E01 - Pilot.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 1u);
	EXPECT_EQ(eps[0].episode_id, (root_ / "S01E01 - Pilot.mkv").string());
	EXPECT_EQ(eps[0].file_path, (root_ / "S01E01 - Pilot.mkv").string());
}

TEST_F(LocalSourceTest, FetchEpisodes_ShowIdIsShowDir)
{
	touch(root_ / "S01E01 - Pilot.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 1u);
	EXPECT_EQ(eps[0].show_id, root_.string());
}

TEST_F(LocalSourceTest, FetchEpisodes_SortedBySeasonThenEpisode)
{
	touch(root_ / "S02E01 - First.mkv");
	touch(root_ / "S01E02 - Second.mkv");
	touch(root_ / "S01E01 - First.mkv");
	const auto eps = src_->fetchEpisodes(root_.string());
	ASSERT_EQ(eps.size(), 3u);
	EXPECT_EQ(eps[0].season, 1);
	EXPECT_EQ(eps[0].episode, 1);
	EXPECT_EQ(eps[1].season, 1);
	EXPECT_EQ(eps[1].episode, 2);
	EXPECT_EQ(eps[2].season, 2);
	EXPECT_EQ(eps[2].episode, 1);
}

TEST_F(LocalSourceTest, FetchEpisodes_EmptyForNonexistentPath)
{
	EXPECT_TRUE(src_->fetchEpisodes("/no/such/show/dir").empty());
}

// ============================================================================
// Source identity
// ============================================================================

TEST(LocalMeta, SourceTypeAndIsSupported)
{
	std::unique_ptr<ConfStore> conf_ = std::make_unique<ConfStore>("./kairos.conf");
	LocalSource src("s1", "/some/path", *conf_);
	EXPECT_EQ(src.sourceType(), "local");
	EXPECT_TRUE(src.isSupported());
}

// ============================================================================
// pushMetadata — NFO writeback (SidecarMetadata write side + LocalSource
// glue). Round-trips a WritebackFields through pushMetadata and back through
// fetchMovies/fetchShows (which read via SidecarMetadata's loadX functions),
// the same path a DB-wipe rescan would take.
// ============================================================================

namespace
{
	WritebackFields makeFields()
	{
		WritebackFields f;
		f.title           = "The Matrix";
		f.overview        = "A hacker discovers reality is a simulation.";
		f.tagline         = "Free your mind";
		f.content_rating  = "R";
		f.genres          = R"(["Action","Sci-Fi"])";
		f.studio          = "Warner Bros.";
		f.director        = "The Wachowskis";
		f.network         = "The CW";
		f.actors          = R"(["Keanu Reeves","Laurence Fishburne"])";
		f.countries       = R"(["United States"])";
		f.release_date    = "1999-03-31";
		f.imdb_id         = "tt0133093";
		f.tmdb_id         = "603";
		f.tvdb_id         = "1234";
		f.audience_rating = 8.7;
		f.match_confirmed = true;
		f.locked          = true;
		return f;
	}
} // namespace

TEST_F(LocalSourceTest, PushMetadata_MovieFolderLayout_WritesNfoAndRoundTrips)
{
	touch(root_ / "movies" / "The Matrix (1999)" / "The.Matrix.1999.mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);

	ASSERT_TRUE(src_->pushMetadata(movies[0].movie_id, "", "movie", makeFields()));
	ASSERT_TRUE(fs::exists(root_ / "movies" / "The Matrix (1999)" / "movie.nfo"));

	const auto reread = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(reread.size(), 1u);
	const auto& m = reread[0];
	EXPECT_EQ(m.title, "The Matrix");
	EXPECT_EQ(m.overview, "A hacker discovers reality is a simulation.");
	EXPECT_EQ(m.tagline, "Free your mind");
	EXPECT_EQ(m.content_rating, "R");
	EXPECT_EQ(m.studio, "Warner Bros.");
	EXPECT_EQ(m.director, "The Wachowskis");
	EXPECT_EQ(m.imdb_id, "tt0133093");
	EXPECT_EQ(m.tmdb_id, "603");
	ASSERT_TRUE(m.year.has_value());
	EXPECT_EQ(*m.year, 1999);
	ASSERT_TRUE(m.audience_rating.has_value());
	EXPECT_FLOAT_EQ(*m.audience_rating, 8.7f);
	EXPECT_TRUE(m.nfo_confirmed);
}

TEST_F(LocalSourceTest, PushMetadata_MovieBareFileLayout_WritesSidecarNfo)
{
	touch(root_ / "movies" / "Inception (2010).mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);

	ASSERT_TRUE(src_->pushMetadata(movies[0].movie_id, "", "movie", makeFields()));
	ASSERT_TRUE(fs::exists(root_ / "movies" / "Inception (2010).nfo"));

	const auto reread = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(reread.size(), 1u);
	EXPECT_EQ(reread[0].tmdb_id, "603");
	EXPECT_TRUE(reread[0].nfo_confirmed);
}

TEST_F(LocalSourceTest, PushMetadata_Show_WritesTvshowNfoAndRoundTrips)
{
	touch(root_ / "shows" / "Breaking Bad" / "Season 1" / "S01E01.mkv");
	const auto shows = src_->fetchShows((root_ / "shows").string());
	ASSERT_EQ(shows.size(), 1u);

	ASSERT_TRUE(src_->pushMetadata(shows[0].show_id, "", "show", makeFields()));
	ASSERT_TRUE(fs::exists(root_ / "shows" / "Breaking Bad" / "tvshow.nfo"));

	const auto reread = src_->fetchShows((root_ / "shows").string());
	ASSERT_EQ(reread.size(), 1u);
	const auto& s = reread[0];
	EXPECT_EQ(s.title, "The Matrix"); // NFO wins over the folder-parsed title
	EXPECT_EQ(s.network, "The CW");
	EXPECT_EQ(s.tvdb_id, "1234");
	EXPECT_EQ(s.tmdb_id, "603");
	EXPECT_TRUE(s.nfo_confirmed);
}

TEST_F(LocalSourceTest, PushMetadata_UnconfirmedFieldsWriteNoConfirmedTag)
{
	touch(root_ / "movies" / "Inception (2010).mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);

	WritebackFields f = makeFields();
	f.match_confirmed = false;
	ASSERT_TRUE(src_->pushMetadata(movies[0].movie_id, "", "movie", f));

	const auto reread = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(reread.size(), 1u);
	EXPECT_FALSE(reread[0].nfo_confirmed);
}

TEST_F(LocalSourceTest, PushMetadata_UnknownItemType_ReturnsFalse)
{
	touch(root_ / "movies" / "Inception (2010).mkv");
	const auto movies = src_->fetchMovies((root_ / "movies").string());
	ASSERT_EQ(movies.size(), 1u);
	EXPECT_FALSE(src_->pushMetadata(movies[0].movie_id, "", "episode", makeFields()));
}

TEST_F(LocalSourceTest, PushMetadata_NonexistentPath_ReturnsFalse)
{
	EXPECT_FALSE(src_->pushMetadata((root_ / "movies" / "Ghost (2000).mkv").string(), "", "movie", makeFields()));
}