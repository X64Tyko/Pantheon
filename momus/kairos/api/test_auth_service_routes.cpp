// Route-level coverage for the profile-switch endpoints added to
// AuthService.cpp (GET /api/auth/profiles, POST /api/auth/switch/:id, PATCH
// /api/users/:id/pin). auth/test_auth_store.cpp already covers the
// AuthStore::switchProfile logic itself in isolation; these tests instead
// spin up a REAL Router (identical wiring to main.cpp) and fire REAL HTTP
// requests at it, the same pattern test_content_service_security.cpp uses —
// the point is to prove the routing/auth-gating/JSON-serialization layer is
// wired correctly, not to re-verify business logic already covered below it.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

#include "api/Router.h"
#include "auth/AuthStore.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include "download/DownloadManager.h"
#include "email/EmailService.h"
#include "log/LogBuffer.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "source/SyncManager.h"

using json = nlohmann::json;

class AuthServiceRoutesTest : public ::testing::Test {
protected:
    Database        db{":memory:"};
    ConfStore       conf{"./momus_auth_routes_test.conf"};
    SyncManager     sync{db, conf};
    RuleEngine      engine{db};
    EPGMaterializer materializer{db, engine};
    DownloadManager dl;
    AuthStore       auth{db};
    EmailService    email{db};
    LogBuffer       logs;

    httplib::Server svr;
    std::unique_ptr<Router>          router;
    std::unique_ptr<httplib::Client> cli;
    std::thread     server_thread;

    std::string admin_id, viewer_id;
    std::string admin_token, viewer_token;

    void SetUp() override {
        router = std::make_unique<Router>(svr, db, sync, conf, logs, engine, materializer, dl, auth, email);
        router->registerRoutes();

        int port = svr.bind_to_any_port("127.0.0.1");
        server_thread = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        cli = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(port));
        cli->set_connection_timeout(5);
        cli->set_read_timeout(5);

        auth.createUser("routes_admin",  "routes-password-1", "admin");
        auth.createUser("routes_viewer", "routes-password-2", "viewer");
        for (const auto& u : auth.listUsers()) {
            if (u.username == "routes_admin")  admin_id  = u.user_id;
            if (u.username == "routes_viewer") viewer_id = u.user_id;
        }
        admin_token  = auth.login("routes_admin",  "routes-password-1");
        viewer_token = auth.login("routes_viewer", "routes-password-2");
    }

    void TearDown() override {
        svr.stop();
        if (server_thread.joinable()) server_thread.join();
    }

    httplib::Headers adminHeaders()  const { return {{"Authorization", "Bearer " + admin_token}}; }
    httplib::Headers viewerHeaders() const { return {{"Authorization", "Bearer " + viewer_token}}; }
};

// GET /api/auth/me is the one Kairos endpoint Hermes calls directly on
// every VOD/preview stream start (authedHephaestusProxy in
// hermes/src/api/Router.cpp) to validate the caller's token before ever
// reaching Hephaestus — Hermes has no session store or auth logic of its
// own, it trusts this response entirely.
TEST_F(AuthServiceRoutesTest, GetMeRequiresAuth) {
    auto r = cli->Get("/api/auth/me");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
}

TEST_F(AuthServiceRoutesTest, GetMeReturnsCurrentUserIdentity) {
    auto r = cli->Get("/api/auth/me", viewerHeaders());
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    json body = json::parse(r->body);
    EXPECT_EQ(body["username"], "routes_viewer");
    EXPECT_EQ(body["role"], "viewer");
    EXPECT_EQ(body["user_id"], viewer_id);
}

// A stale/revoked token is exactly the failure mode Hermes's stream gate
// exists to catch -- must not resolve to any identity.
TEST_F(AuthServiceRoutesTest, GetMeRejectsGarbageToken) {
    auto r = cli->Get("/api/auth/me", httplib::Headers{{"Authorization", "Bearer not-a-real-token"}});
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
}

TEST_F(AuthServiceRoutesTest, GetProfilesRequiresAuth) {
    auto r = cli->Get("/api/auth/profiles");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
}

// The whole point of this endpoint (unlike admin-only GET /api/users) is
// that ANY already-authenticated profile can see the full picker list —
// a regression here would either break the picker for non-admins or leak
// the list to unauthenticated callers.
TEST_F(AuthServiceRoutesTest, GetProfilesVisibleToViewerNotJustAdmin) {
    auto r = cli->Get("/api/auth/profiles", viewerHeaders());
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    json body = json::parse(r->body);
    ASSERT_EQ(body.size(), 2u);
    bool sawAdmin = false, sawViewer = false;
    for (const auto& u : body) {
        if (u["username"] == "routes_admin")  { sawAdmin = true;  EXPECT_FALSE(u["has_pin"]); }
        if (u["username"] == "routes_viewer") { sawViewer = true; EXPECT_FALSE(u["has_pin"]); }
    }
    EXPECT_TRUE(sawAdmin);
    EXPECT_TRUE(sawViewer);
}

TEST_F(AuthServiceRoutesTest, SwitchProfileRequiresAuth) {
    auto r = cli->Post(("/api/auth/switch/" + viewer_id).c_str(), "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
}

TEST_F(AuthServiceRoutesTest, SwitchProfileIntoUnlockedViewerSucceeds) {
    auto r = cli->Post(("/api/auth/switch/" + viewer_id).c_str(), adminHeaders(), "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    json body = json::parse(r->body);
    EXPECT_FALSE(body["token"].get<std::string>().empty());
    EXPECT_EQ(body["user"]["username"], "routes_viewer");
}

// 403, not 401: the caller's own session (viewer_token) is perfectly valid
// here — they're just denied this particular switch by PIN policy. Hades'
// frontend treats any 401 as "session dead, log out everywhere," so this
// status code specifically must not be 401 or a denied switch attempt would
// incorrectly sign the caller out instead of just showing an inline error.
TEST_F(AuthServiceRoutesTest, SwitchProfileIntoAdminWithoutPinDenied) {
    auto r = cli->Post(("/api/auth/switch/" + admin_id).c_str(), viewerHeaders(), "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
    EXPECT_NE(r->body.find("PIN"), std::string::npos);
}

TEST_F(AuthServiceRoutesTest, SetPinRequiresAdmin) {
    json body = {{"pin", "1234"}};
    auto r_noauth = cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), body.dump(), "application/json");
    ASSERT_TRUE(r_noauth);
    EXPECT_EQ(r_noauth->status, 401);

    auto r_viewer = cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), viewerHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(r_viewer);
    EXPECT_EQ(r_viewer->status, 403);
}

TEST_F(AuthServiceRoutesTest, SetPinRejectsBadFormat) {
    json body = {{"pin", "12"}}; // too short
    auto r = cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), adminHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST_F(AuthServiceRoutesTest, SetPinThenSwitchGatesOnCorrectPin) {
    json setBody = {{"pin", "4242"}};
    auto rSet = cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), adminHeaders(), setBody.dump(), "application/json");
    ASSERT_TRUE(rSet);
    ASSERT_EQ(rSet->status, 200);

    // Now visible via the picker as has_pin=true.
    auto rProfiles = cli->Get("/api/auth/profiles", adminHeaders());
    ASSERT_TRUE(rProfiles);
    json profiles = json::parse(rProfiles->body);
    bool foundPin = false;
    for (const auto& u : profiles) if (u["username"] == "routes_viewer") foundPin = u["has_pin"];
    EXPECT_TRUE(foundPin);

    // No pin supplied -> denied (403: the caller's own session stays valid).
    auto rNoPin = cli->Post(("/api/auth/switch/" + viewer_id).c_str(), adminHeaders(), "{}", "application/json");
    ASSERT_TRUE(rNoPin);
    EXPECT_EQ(rNoPin->status, 403);

    // Wrong pin -> denied (403).
    json wrongBody = {{"pin", "0000"}};
    auto rWrong = cli->Post(("/api/auth/switch/" + viewer_id).c_str(), adminHeaders(), wrongBody.dump(), "application/json");
    ASSERT_TRUE(rWrong);
    EXPECT_EQ(rWrong->status, 403);

    // Correct pin -> succeeds.
    json rightBody = {{"pin", "4242"}};
    auto rRight = cli->Post(("/api/auth/switch/" + viewer_id).c_str(), adminHeaders(), rightBody.dump(), "application/json");
    ASSERT_TRUE(rRight);
    EXPECT_EQ(rRight->status, 200);
}

TEST_F(AuthServiceRoutesTest, ClearingPinViaEmptyBodyReopensUnlockedSwitch) {
    json setBody = {{"pin", "5555"}};
    ASSERT_TRUE(cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), adminHeaders(), setBody.dump(), "application/json"));

    json clearBody = {{"pin", ""}};
    auto rClear = cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), adminHeaders(), clearBody.dump(), "application/json");
    ASSERT_TRUE(rClear);
    EXPECT_EQ(rClear->status, 200);

    auto r = cli->Post(("/api/auth/switch/" + viewer_id).c_str(), adminHeaders(), "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
}
