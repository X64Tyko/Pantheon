// Regression test for PlexSyncHelper.cpp's resolveAndExpand: importing a
// Plex/Jellyfin collection that contains a whole show (a common use case —
// grouping an entire series, not just movies/episodes) must expand it to
// every episode rather than silently resolving nothing, since playlist_item
// has no 'show' item_type of its own (episode/movie only — see
// Database.cpp's CHECK). Before this fix, a show-type collection member was
// misclassified as "episode" by the source parsers and so never resolved
// against source_mapping (which stores it under item_type='show'),
// producing a silent synced:0 result.
//
// Same real-PlexSource-against-a-local-TestServer approach as
// test_list_push_helper.cpp (not a hand-rolled fake IMediaSource) so this
// exercises the exact SyncManager::findSource wiring the real API route uses.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "api/services/PlexSyncHelper.h"
#include "db/Database.h"
#include "db/PlaylistRepository.h"
#include "conf/ConfStore.h"
#include "source/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

struct TestServer {
    httplib::Server svr;
    int             port{0};
    std::thread     thread;

    TestServer() { port = svr.bind_to_any_port("127.0.0.1"); }
    void start() {
        thread = std::thread([this] { svr.listen_after_bind(); });
        while (!svr.is_running())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ~TestServer() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

class SyncSourceListItemsTest : public ::testing::Test {
protected:
    fs::path                     db_path_;
    fs::path                     root_;
    std::unique_ptr<Database>    db_;
    std::unique_ptr<ConfStore>   conf_;
    std::unique_ptr<SyncManager> sync_;
    TestServer                   srv_;

    void SetUp() override {
        const auto tag = std::to_string(reinterpret_cast<uintptr_t>(this));
        root_    = fs::temp_directory_path() / ("momus_sscoll_" + tag);
        db_path_ = fs::temp_directory_path() / ("momus_sscoll_" + tag + ".sqlite");
        fs::remove_all(root_);
        fs::create_directories(root_);
        fs::remove(db_path_);

        db_   = std::make_unique<Database>(db_path_.string());
        conf_ = std::make_unique<ConfStore>((root_ / "kairos.conf").string());
        srv_.start();

        SQLite::Statement s(db_->get(),
            "INSERT INTO media_source (source_id, source_type, display_name, base_url) VALUES (?,?,?,?)");
        s.bind(1, "plex1"); s.bind(2, "plex"); s.bind(3, "Plex"); s.bind(4, srv_.url());
        s.exec();
        conf_->setCredentials("plex1", "tok", "");

        sync_ = std::make_unique<SyncManager>(*db_, *conf_);
        sync_->loadSources();
    }

    void TearDown() override {
        db_.reset();
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::remove(db_path_, ec);
    }

    void insertShowWithEpisodes(const std::string& show_id, const std::string& title) {
        SQLite::Statement s(db_->get(), "INSERT INTO show (show_id, title, content_rating) VALUES (?,?,?)");
        s.bind(1, show_id); s.bind(2, title); s.bind(3, std::string(""));
        s.exec();

        auto addEp = [&](const std::string& ep_id, int season, int episode) {
            SQLite::Statement e(db_->get(),
                "INSERT INTO episode (episode_id, show_id, season, episode, title, file_path, duration_ms) "
                "VALUES (?,?,?,?,?,?,1800000)");
            e.bind(1, ep_id); e.bind(2, show_id); e.bind(3, season); e.bind(4, episode);
            e.bind(5, ep_id); e.bind(6, "/x/" + ep_id + ".mkv");
            e.exec();
        };
        addEp(show_id + "-e2", 1, 2);
        addEp(show_id + "-e1", 1, 1);
    }

    void mapShowToSource(const std::string& source_id, const std::string& kairos_id, const std::string& external_id) {
        SQLite::Statement s(db_->get(),
            "INSERT INTO source_mapping (item_type, kairos_id, source_id, external_id) VALUES ('show',?,?,?)");
        s.bind(1, kairos_id); s.bind(2, source_id); s.bind(3, external_id);
        s.exec();
    }
};

TEST_F(SyncSourceListItemsTest, CollectionContainingAWholeShowExpandsToAllItsEpisodes) {
    insertShowWithEpisodes("show1", "Whole Show Collected");
    mapShowToSource("plex1", "show1", "plex-show-1");

    srv_.svr.Get("/library/metadata/col1/children", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"MediaContainer", {{"Metadata", json::array({
            json{{"ratingKey","plex-show-1"},{"type","show"},{"title","Whole Show Collected"}},
        })}}}}.dump(), "application/json");
    });

    PlaylistRepository pr(*db_);
    auto playlist_id = pr.create("Show Collection Import");

    httplib::Response res;
    syncSourceListItems(res, "playlist", playlist_id, "plex1", "col1", "collection", *db_, *sync_);

    auto detail = pr.getDetail(playlist_id);
    ASSERT_TRUE(detail.has_value());
    ASSERT_EQ(detail->items.size(), 2u); // the show's 2 episodes, not 0 and not the show itself
    EXPECT_EQ(detail->items[0].item_type, "episode");
    EXPECT_EQ(detail->items[0].item_id,   "show1-e1"); // season/episode order: E1 before E2
    EXPECT_EQ(detail->items[1].item_id,   "show1-e2");
}

// A mixed collection (some movies, one whole show) — the movie resolves
// directly, the show expands, and both land in the same playlist together.
TEST_F(SyncSourceListItemsTest, MixedMovieAndShowCollectionResolvesBothCorrectly) {
    insertShowWithEpisodes("show1", "Collected Show");
    mapShowToSource("plex1", "show1", "plex-show-1");

    SQLite::Statement m(db_->get(),
        "INSERT INTO movie (movie_id, title, content_rating, file_path, duration_ms, added_at_source) "
        "VALUES ('movie1','Collected Movie','','/x/movie1.mkv',6000000,'local')");
    m.exec();
    SQLite::Statement map(db_->get(),
        "INSERT INTO source_mapping (item_type, kairos_id, source_id, external_id) VALUES ('movie','movie1','plex1','plex-movie-1')");
    map.exec();

    srv_.svr.Get("/library/metadata/col1/children", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"MediaContainer", {{"Metadata", json::array({
            json{{"ratingKey","plex-movie-1"},{"type","movie"},{"title","Collected Movie"},{"duration",6000000}},
            json{{"ratingKey","plex-show-1"},{"type","show"},{"title","Collected Show"}},
        })}}}}.dump(), "application/json");
    });

    PlaylistRepository pr(*db_);
    auto playlist_id = pr.create("Mixed Collection Import");

    httplib::Response res;
    syncSourceListItems(res, "playlist", playlist_id, "plex1", "col1", "collection", *db_, *sync_);

    auto detail = pr.getDetail(playlist_id);
    ASSERT_TRUE(detail.has_value());
    ASSERT_EQ(detail->items.size(), 3u); // 1 movie + 2 expanded episodes
    int movie_count = 0, episode_count = 0;
    for (const auto& item : detail->items) {
        if (item.item_type == "movie")   ++movie_count;
        if (item.item_type == "episode") ++episode_count;
    }
    EXPECT_EQ(movie_count, 1);
    EXPECT_EQ(episode_count, 2);
}

// Regression: a hand-curated collection (e.g. a "Gundam Unicorn order"
// mixing movies/OVAs/specials across shows) commonly references items the
// local library hasn't synced yet — these must be reported by title in the
// response, not just folded into a bare unexplained item-count shortfall.
TEST_F(SyncSourceListItemsTest, UnsyncedItemsAreReportedByTitleNotJustCounted) {
    SQLite::Statement m(db_->get(),
        "INSERT INTO movie (movie_id, title, content_rating, file_path, duration_ms, added_at_source) "
        "VALUES ('movie1','UC Episode 1','','/x/movie1.mkv',6000000,'local')");
    m.exec();
    SQLite::Statement map(db_->get(),
        "INSERT INTO source_mapping (item_type, kairos_id, source_id, external_id) VALUES ('movie','movie1','plex1','plex-uc-1')");
    map.exec();

    srv_.svr.Get("/library/metadata/col1/children", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"MediaContainer", {{"Metadata", json::array({
            json{{"ratingKey","plex-uc-1"},{"type","movie"},{"title","UC Episode 1"},{"duration",6000000}},
            json{{"ratingKey","plex-uc-2"},{"type","movie"},{"title","UC Episode 2 (Not Yet Synced)"},{"duration",6000000}},
        })}}}}.dump(), "application/json");
    });

    PlaylistRepository pr(*db_);
    auto playlist_id = pr.create("Gundam UC Watch Order");

    httplib::Response res;
    syncSourceListItems(res, "playlist", playlist_id, "plex1", "col1", "collection", *db_, *sync_);

    auto body = json::parse(res.body);
    EXPECT_EQ(body.value("synced", -1), 1);
    EXPECT_EQ(body.value("total", -1),  2);
    ASSERT_TRUE(body.contains("unresolved"));
    ASSERT_EQ(body["unresolved"].size(), 1u);
    EXPECT_EQ(body["unresolved"][0].value("title", ""),     "UC Episode 2 (Not Yet Synced)");
    EXPECT_EQ(body["unresolved"][0].value("item_type", ""), "movie");
}
