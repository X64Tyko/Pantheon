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

namespace fs = std::filesystem;

// ============================================================================
// Fixture — creates a unique temp root per test, cleans up in TearDown
// ============================================================================

class LocalSourceTest : public ::testing::Test {
protected:
    fs::path                    root_;
    std::unique_ptr<LocalSource> src_;

    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("momus_local_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(root_);
        fs::create_directories(root_);
        src_ = std::make_unique<LocalSource>("src", root_.string());
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    // Create an empty file at path p, making parent dirs as needed.
    void touch(const fs::path& p) const {
        fs::create_directories(p.parent_path());
        std::ofstream{p}.close();
    }
};

// ============================================================================
// listAvailableLibraries
// ============================================================================

TEST_F(LocalSourceTest, ListLibraries_SkipsHiddenDirs) {
    fs::create_directories(root_ / ".hidden");
    fs::create_directories(root_ / "Movies");
    fs::create_directories(root_ / "TV Shows");
    const auto libs = src_->listAvailableLibraries();
    ASSERT_EQ(libs.size(), 2u);
    // Names are sorted, so: Movies, TV Shows
    EXPECT_EQ(libs[0].name, "Movies");
    EXPECT_EQ(libs[1].name, "TV Shows");
}

TEST_F(LocalSourceTest, ListLibraries_SortedAlphabetically) {
    fs::create_directories(root_ / "Z Library");
    fs::create_directories(root_ / "A Library");
    const auto libs = src_->listAvailableLibraries();
    ASSERT_EQ(libs.size(), 2u);
    EXPECT_EQ(libs[0].name, "A Library");
    EXPECT_EQ(libs[1].name, "Z Library");
}

TEST_F(LocalSourceTest, ListLibraries_FallsBackToRootIfNoSubdirs) {
    // root_ has no subdirectories
    const auto libs = src_->listAvailableLibraries();
    ASSERT_EQ(libs.size(), 1u);
    EXPECT_EQ(libs[0].external_lib_id, root_.string());
    EXPECT_EQ(libs[0].name,            root_.filename().string());
    EXPECT_EQ(libs[0].type,            "mixed");
}

TEST_F(LocalSourceTest, ListLibraries_ExternalIdIsFullPath) {
    fs::create_directories(root_ / "Movies");
    const auto libs = src_->listAvailableLibraries();
    ASSERT_EQ(libs.size(), 1u);
    EXPECT_EQ(libs[0].external_lib_id, (root_ / "Movies").string());
}

TEST_F(LocalSourceTest, ListLibraries_GuessesShowType) {
    // grandchild season dir triggers "show" detection
    fs::create_directories(root_ / "TV" / "Breaking Bad" / "Season 01");
    const auto libs = src_->listAvailableLibraries();
    ASSERT_EQ(libs.size(), 1u);
    EXPECT_EQ(libs[0].type, "show");
}

TEST_F(LocalSourceTest, ListLibraries_GuessesMovieType) {
    // child dir with video files triggers "movie" detection
    touch(root_ / "Movies" / "Inception" / "Inception.mkv");
    const auto libs = src_->listAvailableLibraries();
    ASSERT_EQ(libs.size(), 1u);
    EXPECT_EQ(libs[0].type, "movie");
}

TEST_F(LocalSourceTest, ListLibraries_GuessesMixedForNoContent) {
    fs::create_directories(root_ / "Empty Lib" / "subfolder");
    const auto libs = src_->listAvailableLibraries();
    ASSERT_EQ(libs.size(), 1u);
    EXPECT_EQ(libs[0].type, "mixed");
}

// ============================================================================
// listSubdirectories — path-traversal guard
// ============================================================================

TEST_F(LocalSourceTest, ListSubdirectories_RejectsPathThatEscapesBase) {
    // root_.parent_path() is a sibling or ancestor — must be rejected
    const auto result = src_->listSubdirectories(root_.parent_path().string());
    EXPECT_TRUE(result.empty());
}

TEST_F(LocalSourceTest, ListSubdirectories_ListsNonHiddenSortedSubdirs) {
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

TEST_F(LocalSourceTest, FetchShows_ParsesTitleAndYear) {
    fs::create_directories(root_ / "tv" / "Breaking Bad (2008)");
    const auto shows = src_->fetchShows((root_ / "tv").string());
    ASSERT_EQ(shows.size(), 1u);
    EXPECT_EQ(shows[0].title, "Breaking Bad");
    ASSERT_TRUE(shows[0].year.has_value());
    EXPECT_EQ(*shows[0].year, 2008);
}

TEST_F(LocalSourceTest, FetchShows_TitleWithoutYear) {
    fs::create_directories(root_ / "tv" / "Anime Show");
    const auto shows = src_->fetchShows((root_ / "tv").string());
    ASSERT_EQ(shows.size(), 1u);
    EXPECT_EQ(shows[0].title, "Anime Show");
    EXPECT_FALSE(shows[0].year.has_value());
}

TEST_F(LocalSourceTest, FetchShows_SkipsHiddenDirs) {
    fs::create_directories(root_ / "tv" / ".hidden_show");
    fs::create_directories(root_ / "tv" / "Normal Show");
    const auto shows = src_->fetchShows((root_ / "tv").string());
    ASSERT_EQ(shows.size(), 1u);
    EXPECT_EQ(shows[0].title, "Normal Show");
}

TEST_F(LocalSourceTest, FetchShows_SortedByTitle) {
    fs::create_directories(root_ / "tv" / "Zorro (1990)");
    fs::create_directories(root_ / "tv" / "Archer (2009)");
    const auto shows = src_->fetchShows((root_ / "tv").string());
    ASSERT_EQ(shows.size(), 2u);
    EXPECT_EQ(shows[0].title, "Archer");
    EXPECT_EQ(shows[1].title, "Zorro");
}

TEST_F(LocalSourceTest, FetchShows_ShowIdIsFullPath) {
    fs::create_directories(root_ / "tv" / "Firefly (2002)");
    const auto shows = src_->fetchShows((root_ / "tv").string());
    ASSERT_EQ(shows.size(), 1u);
    EXPECT_EQ(shows[0].show_id, (root_ / "tv" / "Firefly (2002)").string());
}

TEST_F(LocalSourceTest, FetchShows_EmptyForNonexistentPath) {
    EXPECT_TRUE(src_->fetchShows("/nonexistent/path/does/not/exist").empty());
}

// ============================================================================
// fetchMovies
// ============================================================================

TEST_F(LocalSourceTest, FetchMovies_FolderLayout) {
    touch(root_ / "movies" / "The Matrix (1999)" / "The.Matrix.1999.mkv");
    const auto movies = src_->fetchMovies((root_ / "movies").string());
    ASSERT_EQ(movies.size(), 1u);
    EXPECT_EQ(movies[0].title,    "The Matrix");
    ASSERT_TRUE(movies[0].year.has_value());
    EXPECT_EQ(*movies[0].year, 1999);
    EXPECT_EQ(movies[0].file_path, (root_ / "movies" / "The Matrix (1999)" / "The.Matrix.1999.mkv").string());
}

TEST_F(LocalSourceTest, FetchMovies_BareFileLayout) {
    touch(root_ / "movies" / "Inception (2010).mkv");
    const auto movies = src_->fetchMovies((root_ / "movies").string());
    ASSERT_EQ(movies.size(), 1u);
    EXPECT_EQ(movies[0].title, "Inception");
    ASSERT_TRUE(movies[0].year.has_value());
    EXPECT_EQ(*movies[0].year, 2010);
    EXPECT_EQ(movies[0].file_path, (root_ / "movies" / "Inception (2010).mkv").string());
}

TEST_F(LocalSourceTest, FetchMovies_SkipsEmptyFolders) {
    touch(root_ / "movies" / "Empty Folder" / "readme.txt"); // not a video
    const auto movies = src_->fetchMovies((root_ / "movies").string());
    EXPECT_TRUE(movies.empty());
}

TEST_F(LocalSourceTest, FetchMovies_SkipsHiddenFiles) {
    touch(root_ / "movies" / ".hidden.mkv");
    touch(root_ / "movies" / "Real Movie.mkv");
    const auto movies = src_->fetchMovies((root_ / "movies").string());
    ASSERT_EQ(movies.size(), 1u);
    EXPECT_EQ(movies[0].title, "Real Movie");
}

TEST_F(LocalSourceTest, FetchMovies_SortedByTitle) {
    touch(root_ / "movies" / "Zodiac.mkv");
    touch(root_ / "movies" / "Alien.mkv");
    const auto movies = src_->fetchMovies((root_ / "movies").string());
    ASSERT_EQ(movies.size(), 2u);
    EXPECT_EQ(movies[0].title, "Alien");
    EXPECT_EQ(movies[1].title, "Zodiac");
}

TEST_F(LocalSourceTest, FetchMovies_MixedLayouts) {
    touch(root_ / "movies" / "Interstellar (2014)" / "Interstellar.mkv"); // folder layout
    touch(root_ / "movies" / "Dune (2021).mp4");                           // bare file
    const auto movies = src_->fetchMovies((root_ / "movies").string());
    ASSERT_EQ(movies.size(), 2u);
    // sorted: Dune, Interstellar
    EXPECT_EQ(movies[0].title, "Dune");
    EXPECT_EQ(movies[1].title, "Interstellar");
}

// ============================================================================
// fetchEpisodes
// ============================================================================

TEST_F(LocalSourceTest, FetchEpisodes_SeasonDirLayout) {
    touch(root_ / "Season 01" / "S01E01 - Pilot.mkv");
    touch(root_ / "Season 01" / "S01E02 - Bag.mkv");
    touch(root_ / "Season 02" / "S02E01 - Seven.mkv");
    const auto eps = src_->fetchEpisodes(root_.string());
    ASSERT_EQ(eps.size(), 3u);
    // sorted by (season, episode)
    EXPECT_EQ(eps[0].season,  1); EXPECT_EQ(eps[0].episode, 1);
    EXPECT_EQ(eps[0].title,   "Pilot");
    EXPECT_EQ(eps[1].season,  1); EXPECT_EQ(eps[1].episode, 2);
    EXPECT_EQ(eps[1].title,   "Bag");
    EXPECT_EQ(eps[2].season,  2); EXPECT_EQ(eps[2].episode, 1);
    EXPECT_EQ(eps[2].title,   "Seven");
}

TEST_F(LocalSourceTest, FetchEpisodes_FlatLayout) {
    touch(root_ / "S01E01 - Pilot.mkv");
    touch(root_ / "S02E05 - Episode.mkv");
    const auto eps = src_->fetchEpisodes(root_.string());
    ASSERT_EQ(eps.size(), 2u);
    EXPECT_EQ(eps[0].season,  1); EXPECT_EQ(eps[0].episode, 1);
    EXPECT_EQ(eps[1].season,  2); EXPECT_EQ(eps[1].episode, 5);
}

TEST_F(LocalSourceTest, FetchEpisodes_AlternateNotation_1x01) {
    touch(root_ / "Season 01" / "1x05 - Five.mkv");
    const auto eps = src_->fetchEpisodes(root_.string());
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].season,  1);
    EXPECT_EQ(eps[0].episode, 5);
    EXPECT_EQ(eps[0].title,   "Five");
}

TEST_F(LocalSourceTest, FetchEpisodes_AlternateNotation_1X01_Uppercase) {
    touch(root_ / "Season 02" / "2X03 - Three.mkv");
    const auto eps = src_->fetchEpisodes(root_.string());
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].season,  2);
    EXPECT_EQ(eps[0].episode, 3);
}

TEST_F(LocalSourceTest, FetchEpisodes_UnparseableFilenameGetsStemAsTitle) {
    touch(root_ / "RandomFile.mkv");
    const auto eps = src_->fetchEpisodes(root_.string());
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].season,  1);
    EXPECT_EQ(eps[0].episode, 0);
    EXPECT_EQ(eps[0].title,   "RandomFile");
}

TEST_F(LocalSourceTest, FetchEpisodes_SeasonDirParsing_SPrefix) {
    // "S01" shorthand should be recognised as season 1
    touch(root_ / "S01" / "S01E01 - First.mkv");
    const auto eps = src_->fetchEpisodes(root_.string());
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].season, 1);
}

TEST_F(LocalSourceTest, FetchEpisodes_SeasonDirParsing_SeriesPrefix) {
    touch(root_ / "Series 3" / "S03E01 - Start.mkv");
    const auto eps = src_->fetchEpisodes(root_.string());
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].season, 3);
}

TEST_F(LocalSourceTest, FetchEpisodes_EpisodeIdIsFullPath) {
    touch(root_ / "S01E01 - Pilot.mkv");
    const auto eps = src_->fetchEpisodes(root_.string());
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].episode_id, (root_ / "S01E01 - Pilot.mkv").string());
    EXPECT_EQ(eps[0].file_path,  (root_ / "S01E01 - Pilot.mkv").string());
}

TEST_F(LocalSourceTest, FetchEpisodes_ShowIdIsShowDir) {
    touch(root_ / "S01E01 - Pilot.mkv");
    const auto eps = src_->fetchEpisodes(root_.string());
    ASSERT_EQ(eps.size(), 1u);
    EXPECT_EQ(eps[0].show_id, root_.string());
}

TEST_F(LocalSourceTest, FetchEpisodes_SortedBySeasonThenEpisode) {
    touch(root_ / "S02E01 - First.mkv");
    touch(root_ / "S01E02 - Second.mkv");
    touch(root_ / "S01E01 - First.mkv");
    const auto eps = src_->fetchEpisodes(root_.string());
    ASSERT_EQ(eps.size(), 3u);
    EXPECT_EQ(eps[0].season, 1); EXPECT_EQ(eps[0].episode, 1);
    EXPECT_EQ(eps[1].season, 1); EXPECT_EQ(eps[1].episode, 2);
    EXPECT_EQ(eps[2].season, 2); EXPECT_EQ(eps[2].episode, 1);
}

TEST_F(LocalSourceTest, FetchEpisodes_EmptyForNonexistentPath) {
    EXPECT_TRUE(src_->fetchEpisodes("/no/such/show/dir").empty());
}

// ============================================================================
// Source identity
// ============================================================================

TEST(LocalMeta, SourceTypeAndIsSupported) {
    LocalSource src("s1", "/some/path");
    EXPECT_EQ(src.sourceType(), "local");
    EXPECT_TRUE(src.isSupported());
}
