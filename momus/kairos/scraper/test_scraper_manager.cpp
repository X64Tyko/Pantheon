#include <gtest/gtest.h>
#include "db/Database.h"
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

    void SetUp() override {
    }

    void TearDown() override {
        std::filesystem::remove(conf_path);
    }

    void insertShow(const std::string& id, const std::string& title) {
        SQLite::Statement s(db.get(), "INSERT INTO show (show_id, title) VALUES (?,?)");
        s.bind(1, id);
        s.bind(2, title);
        s.exec();
    }

    void insertMovie(const std::string& id, const std::string& title) {
        SQLite::Statement s(db.get(), "INSERT INTO movie (movie_id, title, file_path, duration_ms) VALUES (?,?,'/tmp/movie',0)");
        s.bind(1, id);
        s.bind(2, title);
        s.exec();
    }
};

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
