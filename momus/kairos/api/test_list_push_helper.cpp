// Tests for pushListToSource (ListPushHelper.cpp) — the orchestration layer
// tying a local playlist's items to a real IMediaSource's push methods
// (already unit-tested directly against a mock server in
// source/test_plex_source.cpp's PlexListPush.* suite): resolving each local
// item to the target source's external_id, and choosing create-a-new-remote-
// list vs. reconcile-an-already-linked-one.
//
// Uses a real PlexSource against a local httplib TestServer (same pattern as
// source/test_sync_manager.cpp's real-LocalSource integration tests) rather
// than a hand-rolled fake IMediaSource, so this exercises the exact
// SyncManager::findSource / loadSources wiring the real API route uses.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "api/services/ListPushHelper.h"
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

// ============================================================================
// Local mock Plex server
// ============================================================================

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

class ListPushHelperTest : public ::testing::Test {
protected:
    fs::path                    db_path_;
    fs::path                    root_;
    std::unique_ptr<Database>   db_;
    std::unique_ptr<ConfStore>  conf_;
    std::unique_ptr<SyncManager> sync_;
    TestServer                  srv_;

    void SetUp() override {
        const auto tag = std::to_string(reinterpret_cast<uintptr_t>(this));
        root_    = fs::temp_directory_path() / ("momus_push_" + tag);
        db_path_ = fs::temp_directory_path() / ("momus_push_" + tag + ".sqlite");
        fs::remove_all(root_);
        fs::create_directories(root_);
        fs::remove(db_path_);

        // A real on-disk DB (see SyncManagerTest's identical comment) —
        // SyncManager opens a second connection during sync.
        db_   = std::make_unique<Database>(db_path_.string());
        conf_ = std::make_unique<ConfStore>((root_ / "kairos.conf").string());

        srv_.svr.Get("/identity", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(json{{"MediaContainer", {{"machineIdentifier", "machine-xyz"}}}}.dump(),
                             "application/json");
        });
        srv_.start();

        SQLite::Statement s(db_->get(),
            "INSERT INTO media_source (source_id, source_type, display_name, base_url) VALUES (?,?,?,?)");
        s.bind(1, "plex1"); s.bind(2, "plex"); s.bind(3, "Plex"); s.bind(4, srv_.url());
        s.exec();

        // buildSource() skips a Plex source entirely with no token set — the
        // mock server itself doesn't check auth, but a real token still has
        // to be configured for SyncManager to construct a PlexSource at all.
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

    void insertMovie(const std::string& id, const std::string& title) {
        SQLite::Statement s(db_->get(),
            "INSERT INTO movie (movie_id, title, content_rating, file_path, duration_ms, added_at_source) "
            "VALUES (?,?,?,?,?,?)");
        s.bind(1, id); s.bind(2, title); s.bind(3, std::string(""));
        s.bind(4, "/x/" + id + ".mkv"); s.bind(5, int64_t{6000000}); s.bind(6, std::string("local"));
        s.exec();
    }

    void mapToSource(const std::string& source_id, const std::string& kairos_id, const std::string& external_id) {
        SQLite::Statement s(db_->get(),
            "INSERT INTO source_mapping (item_type, kairos_id, source_id, external_id) VALUES ('movie',?,?,?)");
        s.bind(1, kairos_id); s.bind(2, source_id); s.bind(3, external_id);
        s.exec();
    }
};

TEST_F(ListPushHelperTest, FirstPushCreatesRemoteListAndLinksIt) {
    insertMovie("m1", "Pushed Movie");
    mapToSource("plex1", "m1", "rk1");

    PlaylistRepository pr(*db_);
    auto playlist_id = pr.create("My Pushed Playlist");
    pr.addItem(playlist_id, "movie", "m1");

    std::string captured_target;
    srv_.svr.Post("/playlists", [&](const httplib::Request& req, httplib::Response& res) {
        captured_target = req.target;
        res.set_content(json{{"MediaContainer", {{"Metadata", json::array({json{{"ratingKey","remote-pl-1"}}})}}}}.dump(),
                         "application/json");
    });

    httplib::Response res;
    pushListToSource(res, playlist_id, "plex1", "playlist", "My Pushed Playlist", "", *db_, *sync_);

    EXPECT_NE(res.status, 400); EXPECT_NE(res.status, 404); EXPECT_NE(res.status, 502);
    auto body = json::parse(res.body);
    EXPECT_EQ(body.value("created", false), true);
    EXPECT_EQ(body.value("external_id", ""), "remote-pl-1");
    EXPECT_EQ(body.value("pushed", 0), 1);
    EXPECT_EQ(body.value("unresolved", -1), 0);
    EXPECT_NE(captured_target.find("rk1"), std::string::npos);

    auto link = pr.getLink("playlist", playlist_id);
    ASSERT_TRUE(link.has_value());
    EXPECT_EQ(link->source_id, "plex1");
    EXPECT_EQ(link->external_id, "remote-pl-1");
    EXPECT_EQ(link->plex_type, "playlist");
}

TEST_F(ListPushHelperTest, ItemsNotMappedToThatSourceAreSkippedNotFatal) {
    insertMovie("m1", "Mapped Movie");
    insertMovie("m2", "Unmapped Movie");
    mapToSource("plex1", "m1", "rk1"); // m2 deliberately left unmapped

    PlaylistRepository pr(*db_);
    auto playlist_id = pr.create("Mixed Playlist");
    pr.addItem(playlist_id, "movie", "m1");
    pr.addItem(playlist_id, "movie", "m2");

    srv_.svr.Post("/playlists", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"MediaContainer", {{"Metadata", json::array({json{{"ratingKey","remote-pl-2"}}})}}}}.dump(),
                         "application/json");
    });

    httplib::Response res;
    pushListToSource(res, playlist_id, "plex1", "playlist", "Mixed Playlist", "", *db_, *sync_);

    EXPECT_NE(res.status, 400); EXPECT_NE(res.status, 404); EXPECT_NE(res.status, 502);
    auto body = json::parse(res.body);
    EXPECT_EQ(body.value("pushed", 0), 1);      // only m1
    EXPECT_EQ(body.value("unresolved", -1), 1); // m2 skipped, not fatal
}

TEST_F(ListPushHelperTest, SecondPushToSameLinkReconcilesInsteadOfRecreating) {
    insertMovie("m1", "Keep");
    insertMovie("m2", "New");
    mapToSource("plex1", "m1", "rk-keep");
    mapToSource("plex1", "m2", "rk-new");

    PlaylistRepository pr(*db_);
    auto playlist_id = pr.create("Reconciled Playlist");
    pr.addItem(playlist_id, "movie", "m1");
    pr.addItem(playlist_id, "movie", "m2");
    // Pre-existing link to a remote list that currently has rk-keep AND
    // rk-stale (something the local playlist no longer references).
    pr.upsertPlexLink("playlist", playlist_id, "plex1", "remote-existing", "playlist");

    srv_.svr.Get("/playlists/remote-existing/items", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"MediaContainer", {{"Metadata", json::array({
            json{{"ratingKey","rk-keep"},{"type","movie"}},
            json{{"ratingKey","rk-stale"},{"type","movie"},{"playlistItemID","entry-stale"}},
        })}}}}.dump(), "application/json");
    });
    std::string add_target, remove_path;
    srv_.svr.Put("/playlists/remote-existing/items", [&](const httplib::Request& req, httplib::Response& res) {
        add_target = req.target; res.status = 200;
    });
    srv_.svr.Delete("/playlists/remote-existing/items/entry-stale", [&](const httplib::Request& req, httplib::Response& res) {
        remove_path = req.path; res.status = 200;
    });
    // createRemoteList must NOT be called this time — registering it would
    // make an accidental re-create pass the test, so deliberately absent;
    // httplib returns 404 for any unregistered route, which the assertion
    // on res.status below would catch as a failure if it were hit instead.

    httplib::Response res;
    pushListToSource(res, playlist_id, "plex1", "playlist", "Reconciled Playlist", "", *db_, *sync_);

    EXPECT_NE(res.status, 400); EXPECT_NE(res.status, 404); EXPECT_NE(res.status, 502);
    auto body = json::parse(res.body);
    EXPECT_EQ(body.value("created", true), false);
    EXPECT_EQ(body.value("external_id", ""), "remote-existing");
    EXPECT_EQ(body.value("added", -1), 1);   // rk-new
    EXPECT_EQ(body.value("removed", -1), 1); // rk-stale

    EXPECT_NE(add_target.find("rk-new"), std::string::npos);
    EXPECT_EQ(remove_path, "/playlists/remote-existing/items/entry-stale");
}

TEST_F(ListPushHelperTest, NoResolvedItemsReturns400) {
    insertMovie("m1", "Never Synced From Plex");
    // deliberately no mapToSource call

    PlaylistRepository pr(*db_);
    auto playlist_id = pr.create("Unpushable Playlist");
    pr.addItem(playlist_id, "movie", "m1");

    httplib::Response res;
    pushListToSource(res, playlist_id, "plex1", "playlist", "Unpushable Playlist", "", *db_, *sync_);
    EXPECT_EQ(res.status, 400);
}

TEST_F(ListPushHelperTest, UnknownSourceReturns404) {
    PlaylistRepository pr(*db_);
    auto playlist_id = pr.create("Orphan Playlist");

    httplib::Response res;
    pushListToSource(res, playlist_id, "does-not-exist", "playlist", "Orphan Playlist", "", *db_, *sync_);
    EXPECT_EQ(res.status, 404);
}
