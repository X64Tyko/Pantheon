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
#include "db/ConfigRepository.h"
#include "db/Database.h"
#include "download/DownloadManager.h"
#include "email/EmailService.h"
#include "log/LogBuffer.h"
#include "backup/BackupManager.h"
#include "jobs/JobScheduler.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "source/SyncManager.h"

using json = nlohmann::json;

class AuthServiceRoutesTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ConfStore conf{"./momus_auth_routes_test.conf"};
	SyncManager sync{db, conf};
	RuleEngine engine{db};
	EPGMaterializer materializer{db, engine};
	DownloadManager dl;
	AuthStore auth{db};
	EmailService email{db};
	LogBuffer logs;
	JobScheduler jobs;
	BackupManager backups{db, "", "", "/tmp/kairos_test_backups_unused"};

	httplib::Server svr;
	std::unique_ptr<Router> router;
	std::unique_ptr<httplib::Client> cli;
	std::thread server_thread;

	std::string admin_id, viewer_id;
	std::string admin_token, viewer_token;

	void SetUp() override
	{
		router = std::make_unique<Router>(svr, db, sync, conf, logs, engine, materializer, dl, auth, email, jobs, backups);
		router->registerRoutes();

		int port      = svr.bind_to_any_port("127.0.0.1");
		server_thread = std::thread([this] { svr.listen_after_bind(); });
		for (int i = 0; i < 200 && !svr.is_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));

		cli = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(port));
		cli->set_connection_timeout(5);
		cli->set_read_timeout(5);

		auth.createUser("routes_admin", "routes-password-1", "admin");
		auth.createUser("routes_viewer", "routes-password-2", "viewer");
		for (const auto& u : auth.listUsers())
		{
			if (u.username == "routes_admin") admin_id = u.user_id;
			if (u.username == "routes_viewer") viewer_id = u.user_id;
		}
		admin_token  = auth.login("routes_admin", "routes-password-1");
		viewer_token = auth.login("routes_viewer", "routes-password-2");
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
	}

	httplib::Headers adminHeaders() const { return {{"Authorization", "Bearer " + admin_token}}; }
	httplib::Headers viewerHeaders() const { return {{"Authorization", "Bearer " + viewer_token}}; }
};

// GET /api/auth/me is the one Kairos endpoint Hermes calls directly on
// every VOD/preview stream start (authedHephaestusProxy in
// hermes/src/api/Router.cpp) to validate the caller's token before ever
// reaching Hephaestus — Hermes has no session store or auth logic of its
// own, it trusts this response entirely.
TEST_F(AuthServiceRoutesTest, GetMeRequiresAuth)
{
	auto r = cli->Get("/api/auth/me");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(AuthServiceRoutesTest, GetMeReturnsCurrentUserIdentity)
{
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
TEST_F(AuthServiceRoutesTest, GetMeRejectsGarbageToken)
{
	auto r = cli->Get("/api/auth/me", httplib::Headers{{"Authorization", "Bearer not-a-real-token"}});
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

// POST /api/auth/login previously had no rate limiting at all — see
// auth/test_auth_store.cpp for the AuthStore::login lockout logic itself in
// isolation; this proves it's actually wired up through the route (same
// generic 401 either way, so a script probing this endpoint can't
// distinguish "wrong password" from "locked out" and use that as an oracle).
TEST_F(AuthServiceRoutesTest, LoginLocksOutAfterFiveWrongAttemptsThroughTheRoute)
{
	for (int i = 0; i < 5; ++i)
	{
		json body = {{"username", "routes_admin"}, {"password", "wrong-password"}};
		auto r    = cli->Post("/api/auth/login", body.dump(), "application/json");
		ASSERT_TRUE(r);
		EXPECT_EQ(r->status, 401);
	}
	json correct = {{"username", "routes_admin"}, {"password", "routes-password-1"}};
	auto r       = cli->Post("/api/auth/login", correct.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401); // still locked out even with the right password
}

TEST_F(AuthServiceRoutesTest, GetProfilesRequiresAuth)
{
	auto r = cli->Get("/api/auth/profiles");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

// The whole point of this endpoint (unlike admin-only GET /api/users) is
// that ANY already-authenticated profile can see the full picker list —
// a regression here would either break the picker for non-admins or leak
// the list to unauthenticated callers.
TEST_F(AuthServiceRoutesTest, GetProfilesVisibleToViewerNotJustAdmin)
{
	auto r = cli->Get("/api/auth/profiles", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	json body = json::parse(r->body);
	ASSERT_EQ(body.size(), 2u);
	bool sawAdmin = false, sawViewer = false;
	for (const auto& u : body)
	{
		if (u["username"] == "routes_admin")
		{
			sawAdmin = true;
			EXPECT_FALSE(u["has_pin"]);
		}
		if (u["username"] == "routes_viewer")
		{
			sawViewer = true;
			EXPECT_FALSE(u["has_pin"]);
		}
	}
	EXPECT_TRUE(sawAdmin);
	EXPECT_TRUE(sawViewer);
}

// GET /api/auth/profiles is reachable by ANY authenticated session, including
// a freshly self-created guest — it previously returned full userJson() for
// every account (must_change_password, last_seen, rating ceilings, language/
// landing-page preferences), letting a guest map out real accounts' details
// server-wide. Must be trimmed to exactly what the picker UI renders (see
// profilePickerJson in AuthService.cpp): user_id/username/role/restricted/
// has_pin, nothing else.
TEST_F(AuthServiceRoutesTest, GetProfilesOmitsSensitiveFieldsFromEveryEntry)
{
	auto r = cli->Get("/api/auth/profiles", viewerHeaders());
	ASSERT_TRUE(r);
	ASSERT_EQ(r->status, 200);
	json body = json::parse(r->body);
	ASSERT_GE(body.size(), 1u);
	for (const auto& u : body)
	{
		EXPECT_TRUE(u.contains("user_id"));
		EXPECT_TRUE(u.contains("username"));
		EXPECT_TRUE(u.contains("role"));
		EXPECT_TRUE(u.contains("restricted"));
		EXPECT_TRUE(u.contains("has_pin"));
		EXPECT_FALSE(u.contains("must_change_password"));
		EXPECT_FALSE(u.contains("last_seen"));
		EXPECT_FALSE(u.contains("max_tv_rating"));
		EXPECT_FALSE(u.contains("max_movie_rating"));
		EXPECT_FALSE(u.contains("max_channel_rating"));
		EXPECT_FALSE(u.contains("default_audio_lang"));
		EXPECT_FALSE(u.contains("default_subtitle_lang"));
		EXPECT_FALSE(u.contains("default_landing_page"));
		EXPECT_FALSE(u.contains("is_guest"));
	}
}

// GET /api/users (admin-only) is a completely separate endpoint from
// /api/auth/profiles and must keep returning the full detail admin
// management actually needs — proves the trim above is scoped to the picker
// endpoint only, not a blanket change to userJson() itself.
TEST_F(AuthServiceRoutesTest, GetUsersAdminEndpointStillReturnsFullDetail)
{
	auto r = cli->Get("/api/users", adminHeaders());
	ASSERT_TRUE(r);
	ASSERT_EQ(r->status, 200);
	json body = json::parse(r->body);
	ASSERT_GE(body.size(), 1u);
	for (const auto& u : body) EXPECT_TRUE(u.contains("must_change_password"));
}

TEST_F(AuthServiceRoutesTest, SwitchProfileRequiresAuth)
{
	auto r = cli->Post(("/api/auth/switch/" + viewer_id).c_str(), "{}", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(AuthServiceRoutesTest, SwitchProfileIntoUnlockedViewerSucceeds)
{
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
TEST_F(AuthServiceRoutesTest, SwitchProfileIntoAdminWithoutPinDenied)
{
	auto r = cli->Post(("/api/auth/switch/" + admin_id).c_str(), viewerHeaders(), "{}", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
	EXPECT_NE(r->body.find("PIN"), std::string::npos);
}

TEST_F(AuthServiceRoutesTest, SetPinRequiresAdmin)
{
	json body     = {{"pin", "1234"}};
	auto r_noauth = cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), body.dump(), "application/json");
	ASSERT_TRUE(r_noauth);
	EXPECT_EQ(r_noauth->status, 401);

	auto r_viewer = cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), viewerHeaders(), body.dump(), "application/json");
	ASSERT_TRUE(r_viewer);
	EXPECT_EQ(r_viewer->status, 403);
}

TEST_F(AuthServiceRoutesTest, SetPinRejectsBadFormat)
{
	json body = {{"pin", "12"}}; // too short
	auto r    = cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), adminHeaders(), body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 400);
}

TEST_F(AuthServiceRoutesTest, SetPinThenSwitchGatesOnCorrectPin)
{
	json setBody = {{"pin", "4242"}};
	auto rSet    = cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), adminHeaders(), setBody.dump(), "application/json");
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
	auto rWrong    = cli->Post(("/api/auth/switch/" + viewer_id).c_str(), adminHeaders(), wrongBody.dump(), "application/json");
	ASSERT_TRUE(rWrong);
	EXPECT_EQ(rWrong->status, 403);

	// Correct pin -> succeeds.
	json rightBody = {{"pin", "4242"}};
	auto rRight    = cli->Post(("/api/auth/switch/" + viewer_id).c_str(), adminHeaders(), rightBody.dump(), "application/json");
	ASSERT_TRUE(rRight);
	EXPECT_EQ(rRight->status, 200);
}

TEST_F(AuthServiceRoutesTest, ClearingPinViaEmptyBodyReopensUnlockedSwitch)
{
	json setBody = {{"pin", "5555"}};
	ASSERT_TRUE(cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), adminHeaders(), setBody.dump(), "application/json"));

	json clearBody = {{"pin", ""}};
	auto rClear    = cli->Patch(("/api/users/" + viewer_id + "/pin").c_str(), adminHeaders(), clearBody.dump(), "application/json");
	ASSERT_TRUE(rClear);
	EXPECT_EQ(rClear->status, 200);

	auto r = cli->Post(("/api/auth/switch/" + viewer_id).c_str(), adminHeaders(), "{}", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
}

// ---------------------------------------------------------------------------
// POST /api/auth/guest / PATCH /api/auth/me/guest / DELETE /api/auth/me/guest
// ---------------------------------------------------------------------------

TEST_F(AuthServiceRoutesTest, PostGuestFailsWhenGuestProfilesDisabled)
{
	// guest_profiles_enabled defaults off — never explicitly enabled here.
	json body = {{"display_name", "Curious Visitor"}};
	auto r    = cli->Post("/api/auth/guest", body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(AuthServiceRoutesTest, PostGuestRequiresNoAuthHeaderAtAllWhenEnabled)
{
	ConfigRepository(db).setValue("guest_profiles_enabled", "1");
	json body = {{"display_name", "Curious Visitor"}};
	// Deliberately no Authorization header — this is the whole point, a
	// visitor with no session yet.
	auto r = cli->Post("/api/auth/guest", body.dump(), "application/json");
	ASSERT_TRUE(r);
	ASSERT_EQ(r->status, 200);
	auto j = json::parse(r->body);
	EXPECT_FALSE(j["token"].get<std::string>().empty());
	EXPECT_EQ(j["user"]["role"], "viewer");
	EXPECT_TRUE(j["user"]["is_guest"].get<bool>());
}

TEST_F(AuthServiceRoutesTest, PostGuestRejectsEmptyDisplayName)
{
	ConfigRepository(db).setValue("guest_profiles_enabled", "1");
	json body = {{"display_name", ""}};
	auto r    = cli->Post("/api/auth/guest", body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 400);
}

TEST_F(AuthServiceRoutesTest, PostGuestRejectsOnceMaxConcurrentIsReached)
{
	ConfigRepository(db).setValue("guest_profiles_enabled", "1");
	ConfigRepository(db).setValue("guest_max_concurrent", "1");

	json first = {{"display_name", "First Guest"}};
	auto r1    = cli->Post("/api/auth/guest", first.dump(), "application/json");
	ASSERT_TRUE(r1);
	ASSERT_EQ(r1->status, 200);

	json second = {{"display_name", "Second Guest"}};
	auto r2     = cli->Post("/api/auth/guest", second.dump(), "application/json");
	ASSERT_TRUE(r2);
	EXPECT_EQ(r2->status, 429);
}

// Independent of the concurrent-guest cap above (which stops N guests
// existing at once but not a script creating-and-abandoning accounts as fast
// as the server can hash a password) — POST /api/auth/guest is now also
// per-IP rate-limited (see AuthService.h's guest_creation_limiter_), 10 per
// 10 minutes. guest_max_concurrent is set high here specifically so this
// test exercises the rate limiter, not the concurrency cap already covered
// above.
TEST_F(AuthServiceRoutesTest, PostGuestRateLimitedPerIpIndependentOfConcurrentCap)
{
	ConfigRepository(db).setValue("guest_profiles_enabled", "1");
	ConfigRepository(db).setValue("guest_max_concurrent", "100");

	int ok_count = 0, limited_count = 0;
	for (int i = 0; i < 11; ++i)
	{
		json body = {{"display_name", "Rate Limit Guest " + std::to_string(i)}};
		auto r    = cli->Post("/api/auth/guest", body.dump(), "application/json");
		ASSERT_TRUE(r);
		if (r->status == 200) ++ok_count;
		else if (r->status == 429) ++limited_count;
	}
	// All 11 requests come from the same test client (127.0.0.1, no
	// X-Forwarded-For), so they share one rate-limit bucket — the 11th
	// (limit is 10) must be throttled.
	EXPECT_EQ(ok_count, 10);
	EXPECT_EQ(limited_count, 1);
}

TEST_F(AuthServiceRoutesTest, PatchMeGuestForbiddenForARegularViewer)
{
	json body = {{"pin", "1234"}};
	auto r    = cli->Patch("/api/auth/me/guest", viewerHeaders(), body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(AuthServiceRoutesTest, PatchMeGuestRejectedWithNoAuthAtAll)
{
	// /api/auth/me/guest is NOT in Router.cpp's isPublicPath (unlike
	// /api/auth/guest) — a request with no token at all never reaches this
	// route's own is_guest check; it's rejected by Router's pre-routing auth
	// gate first, same as any other authenticated route with no credential.
	json body = {{"pin", "1234"}};
	auto r    = cli->Patch("/api/auth/me/guest", body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(AuthServiceRoutesTest, PatchMeGuestSucceedsForAGuestAndActuallyAppliesRestriction)
{
	ConfigRepository(db).setValue("guest_profiles_enabled", "1");
	json create = {{"display_name", "Configurable Guest"}};
	auto rc     = cli->Post("/api/auth/guest", create.dump(), "application/json");
	ASSERT_TRUE(rc);
	auto guest_token = json::parse(rc->body)["token"].get<std::string>();

	json setup = {{"pin", "9876"}, {"restricted", true}, {"max_movie_rating", "PG-13"}};
	auto r     = cli->Patch("/api/auth/me/guest", {{"Authorization", "Bearer " + guest_token}}, setup.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);

	auto me = cli->Get("/api/auth/me", {{"Authorization", "Bearer " + guest_token}});
	ASSERT_TRUE(me);
	auto j = json::parse(me->body);
	EXPECT_TRUE(j["restricted"].get<bool>());
	EXPECT_EQ(j["max_movie_rating"], "PG-13");
	EXPECT_TRUE(j["has_pin"].get<bool>());
}

TEST_F(AuthServiceRoutesTest, DeleteMeGuestForbiddenForARegularViewer)
{
	auto r = cli->Delete("/api/auth/me/guest", viewerHeaders());
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
	// The account must still exist.
	auto me = cli->Get("/api/auth/me", viewerHeaders());
	ASSERT_TRUE(me);
	EXPECT_EQ(me->status, 200);
}

TEST_F(AuthServiceRoutesTest, DeleteMeGuestSucceedsForAGuestAndTheTokenStopsWorking)
{
	ConfigRepository(db).setValue("guest_profiles_enabled", "1");
	json create = {{"display_name", "Self Deleting Guest"}};
	auto rc     = cli->Post("/api/auth/guest", create.dump(), "application/json");
	ASSERT_TRUE(rc);
	auto guest_token = json::parse(rc->body)["token"].get<std::string>();

	auto rd = cli->Delete("/api/auth/me/guest", {{"Authorization", "Bearer " + guest_token}});
	ASSERT_TRUE(rd);
	EXPECT_EQ(rd->status, 200);

	auto me = cli->Get("/api/auth/me", {{"Authorization", "Bearer " + guest_token}});
	ASSERT_TRUE(me);
	EXPECT_EQ(me->status, 401);
}

TEST_F(AuthServiceRoutesTest, GetPublicSettingsExposesOnlyTheEnabledFlagNotTheAdminTunables)
{
	ConfigRepository(db).setValue("guest_profiles_enabled", "1");
	ConfigRepository(db).setValue("guest_max_concurrent", "5");
	auto r = cli->Get("/api/config/public-settings");
	ASSERT_TRUE(r);
	ASSERT_EQ(r->status, 200);
	auto j = json::parse(r->body);
	EXPECT_TRUE(j["guest_profiles_enabled"].get<bool>());
	EXPECT_FALSE(j.contains("guest_max_concurrent"));
	EXPECT_FALSE(j.contains("guest_idle_timeout_days"));
}

// ---------------------------------------------------------------------------
// require_admin_password_switch — general security hardening (independent of
// guest profiles, but auto-enabled the moment they turn on).
// ---------------------------------------------------------------------------

TEST_F(AuthServiceRoutesTest, RequireAdminPasswordSwitch_RejectsPinEvenWhenCorrectlyConfigured)
{
	ConfigRepository(db).setValue("require_admin_password_switch", "1");
	json setPin = {{"pin", "1234"}};
	ASSERT_EQ(200, cli->Patch(("/api/users/" + admin_id + "/pin").c_str(), adminHeaders(), setPin.dump(), "application/json")->status);

	json body = {{"pin", "1234"}};
	auto r    = cli->Post(("/api/auth/switch/" + admin_id).c_str(), adminHeaders(), body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(AuthServiceRoutesTest, RequireAdminPasswordSwitch_OffByDefaultAllowsCorrectAdminPin)
{
	json setPin = {{"pin", "1234"}};
	ASSERT_EQ(200, cli->Patch(("/api/users/" + admin_id + "/pin").c_str(), adminHeaders(), setPin.dump(), "application/json")->status);

	json body = {{"pin", "1234"}};
	auto r    = cli->Post(("/api/auth/switch/" + admin_id).c_str(), adminHeaders(), body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
}

TEST_F(AuthServiceRoutesTest, RequireAdminPasswordSwitch_AutoEnabledWhenGuestProfilesTurnOn)
{
	json body = {{"guest_profiles_enabled", true}};
	auto r    = cli->Patch("/api/config/settings", adminHeaders(), body.dump(), "application/json");
	ASSERT_TRUE(r);
	ASSERT_EQ(r->status, 200);
	auto j = json::parse(r->body);
	EXPECT_TRUE(j["require_admin_password_switch"].get<bool>());
}

TEST_F(AuthServiceRoutesTest, RequireAdminPasswordSwitch_TurningGuestProfilesOffDoesNotAutoRevertIt)
{
	json enableGuests = {{"guest_profiles_enabled", true}};
	ASSERT_EQ(200, cli->Patch("/api/config/settings", adminHeaders(), enableGuests.dump(), "application/json")->status);

	json disableGuests = {{"guest_profiles_enabled", false}};
	auto r             = cli->Patch("/api/config/settings", adminHeaders(), disableGuests.dump(), "application/json");
	ASSERT_TRUE(r);
	ASSERT_EQ(r->status, 200);
	auto j = json::parse(r->body);
	// The raw stored setting stays on — turning guests off is one-directional,
	// it never silently re-weakens a setting it previously turned on.
	EXPECT_TRUE(j["require_admin_password_switch"].get<bool>());
}

TEST_F(AuthServiceRoutesTest, RequireAdminPasswordSwitch_GuestProfilesOnForcesEnforcementEvenIfRawSettingIsOff)
{
	// Never explicitly set require_admin_password_switch — only guests.
	ConfigRepository(db).setValue("guest_profiles_enabled", "1");
	json setPin = {{"pin", "1234"}};
	ASSERT_EQ(200, cli->Patch(("/api/users/" + admin_id + "/pin").c_str(), adminHeaders(), setPin.dump(), "application/json")->status);

	json body = {{"pin", "1234"}};
	auto r    = cli->Post(("/api/auth/switch/" + admin_id).c_str(), adminHeaders(), body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(AuthServiceRoutesTest, RequireAdminPasswordSwitch_PublicSettingsExposesTheEffectiveNotRawValue)
{
	// guest_profiles_enabled on, require_admin_password_switch never
	// explicitly set — public-settings must still report the *effective*
	// (OR'd) true, since that's what the picker uses to decide whether to
	// prompt an admin tile for a PIN or a password at all.
	ConfigRepository(db).setValue("guest_profiles_enabled", "1");
	auto r = cli->Get("/api/config/public-settings");
	ASSERT_TRUE(r);
	auto j = json::parse(r->body);
	EXPECT_TRUE(j["require_admin_password_switch"].get<bool>());
}