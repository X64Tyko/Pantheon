#include <gtest/gtest.h>
#include "db/Database.h"
#include "db/ContentRepository.h"
#include "conf/ConfStore.h"
#include "scraper/ScraperManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <filesystem>

class ScraperManagerTest : public ::testing::Test {
protected:
    Database db{":memory:"};
    // Use a temp file for ConfStore to avoid creating accidental ":memory:" files in the cwd.
    std::string conf_path = (std::filesystem::temp_directory_path() / "momus_conf_test").string();
    ConfStore conf{conf_path};
    ScraperManager manager{db, conf};
    ContentRepository repo{db};

    void SetUp() override {
    }

    void TearDown() override {
        std::filesystem::remove(conf_path);
    }

    void insertShow(const std::string& id, const std::string& title, const std::string& thumb = "", const std::string& art = "") {
        SQLite::Statement s(db.get(), "INSERT INTO show (show_id, title, thumb, art) VALUES (?,?,?,?)");
        s.bind(1, id);
        s.bind(2, title);
        s.bind(3, thumb);
        s.bind(4, art);
        s.exec();
    }

    void insertMovie(const std::string& id, const std::string& title, const std::string& thumb = "", const std::string& art = "") {
        SQLite::Statement s(db.get(), "INSERT INTO movie (movie_id, title, file_path, duration_ms, thumb, art) VALUES (?,?,'/tmp/movie',0,?,?)");
        s.bind(1, id);
        s.bind(2, title);
        s.bind(3, thumb);
        s.bind(4, art);
        s.exec();
    }
};

TEST_F(ScraperManagerTest, ImagePersistenceShow) {
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

TEST_F(ScraperManagerTest, ImagePersistenceMovie) {
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

TEST_F(ScraperManagerTest, ImagePersistenceEpisode) {
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

TEST_F(ScraperManagerTest, SetAndGetExternalIds) {
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

TEST_F(ScraperManagerTest, AlternateTitles) {
    insertShow("s1", "Show 1");
    
    std::vector<std::string> titles = {"Title A", "Title B"};
    manager.setAlternateTitles("s1", "show", titles);
    
    auto loaded = manager.getAlternateTitles("s1", "show");
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_TRUE(std::find(loaded.begin(), loaded.end(), "Title A") != loaded.end());
    EXPECT_TRUE(std::find(loaded.begin(), loaded.end(), "Title B") != loaded.end());
}

TEST_F(ScraperManagerTest, PriorityOrderingOnUpdate) {
    insertShow("s1", "Show 1");
    
    // Initial set - testing that it sorts by priority ASC
    manager.setExternalIds("s1", "show", {{"tvdb", "456", 2}, {"tmdb", "123", 1}});
    
    // Check they are returned sorted by priority
    auto loaded = manager.getExternalIds("s1", "show");
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].source, "tmdb"); // priority 1
    EXPECT_EQ(loaded[1].source, "tvdb"); // priority 2
}

TEST_F(ScraperManagerTest, MovieExternalIds) {
    insertMovie("m1", "Movie 1");
    
    manager.setExternalIds("m1", "movie", {{"tmdb", "m123", 1}});
    auto loaded = manager.getExternalIds("m1", "movie");
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].source, "tmdb");
}

TEST_F(ScraperManagerTest, LanguageWeightSettings) {
    ScraperSettings s = manager.getSettings();
    
    // Find TMDB config
    auto it = std::find_if(s.configs.begin(), s.configs.end(), [](const auto& c) { return c.source == "tmdb"; });
    ASSERT_NE(it, s.configs.end());
    
    it->language_weight = 0.5;
    it->enabled = true;
    manager.updateSettings(s);
    
    ScraperSettings s2 = manager.getSettings();
    auto it2 = std::find_if(s2.configs.begin(), s2.configs.end(), [](const auto& c) { return c.source == "tmdb"; });
    // std::stod(std::to_string(0.5)) might have slight precision diff, but 0.5 is exact in float
    EXPECT_NEAR(it2->language_weight, 0.5, 0.0001);
    EXPECT_TRUE(it2->enabled);
}
