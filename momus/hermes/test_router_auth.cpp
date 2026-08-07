// Router.cpp's authedHephaestusProxy (POST /stream/vod|preview/...) is the
// one place on Hermes that stands between an anonymous LAN client and the
// private library: unlike /stream/channels/:id (open on purpose, for
// HDHomeRun/IPTV client compatibility), this route asks Kairos to validate
// the caller's bearer token before proxying to Hephaestus, and — for
// start/switch calls specifically — asks Kairos a second time whether
// parental-controls restrictions allow this exact content, with a deliberate
// fail-open-on-infrastructure-error / fail-closed-on-explicit-denial policy
// (see the comment right above authedHephaestusProxy's definition). None of
// that had any test coverage before this file — this is the first Hermes
// test that stands up the real registerRoutes() router end-to-end (a
// MockKairos standing in for both the auth check and the access-check calls,
// same real-local-httplib::Server pattern test_channel_broadcaster.cpp
// already established for MockHephaestus) rather than testing an isolated
// class.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

#include "../../hermes/src/api/Router.h"
#include "../../hermes/src/broadcast/BroadcasterManager.h"
#include "../../hermes/src/devices/DeviceSessionManager.h"
#include "../../hermes/src/kairos/KairosClient.h"
#include "../../hermes/src/watchtogether/WatchTogetherManager.h"
#include "log/LogBuffer.h"

using json = nlohmann::json;

namespace
{
	// Stands in for both Kairos (auth/access-check) and Hephaestus (the
	// proxy target) — Router.cpp's proxyRequest() just forwards to whatever
	// cfg.hephaestus_url points at, so one mock server can play both roles
	// as long as the path sets don't collide, which they don't here.
	class MockUpstream
	{
	public:
		// nullopt means "no handler registered" -- httplib answers with its
		// own 404, which is exactly what "unreachable/misconfigured" looks
		// like to the caller for the purposes of the fail-open assertion.
		std::optional<std::pair<int, json>> auth_me_response;
		std::optional<std::pair<int, json>> access_check_response;

		void start()
		{
			svr.Get("/api/auth/me", [this](const httplib::Request&, httplib::Response& res)
			{
				if (!auth_me_response) return;
				res.status = auth_me_response->first;
				res.set_content(auth_me_response->second.dump(), "application/json");
			});
			svr.Get(R"(/api/content/.*/access-check)", [this](const httplib::Request&, httplib::Response& res)
			{
				if (!access_check_response) return;
				res.status = access_check_response->first;
				res.set_content(access_check_response->second.dump(), "application/json");
			});
			svr.Get(R"(/api/channels/.*/access-check)", [this](const httplib::Request&, httplib::Response& res)
			{
				if (!access_check_response) return;
				res.status = access_check_response->first;
				res.set_content(access_check_response->second.dump(), "application/json");
			});
			// Stand-in Hephaestus target -- proves a request actually got
			// proxied through (as opposed to being rejected 401/403 before
			// ever reaching proxyRequest).
			auto proxied = [](const httplib::Request&, httplib::Response& res)
			{
				res.status = 200;
				res.set_content("proxied-ok", "text/plain");
			};
			svr.Post(R"(/stream/vod/.*)", proxied);
			svr.Post(R"(/stream/preview/.*)", proxied);

			port = svr.bind_to_any_port("127.0.0.1");
			url  = "http://127.0.0.1:" + std::to_string(port);
			t    = std::thread([this] { svr.listen_after_bind(); });
			for (int i = 0; i < 200 && !svr.is_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		~MockUpstream()
		{
			svr.stop();
			if (t.joinable()) t.join();
		}

		std::string url;
		int port = 0;
		httplib::Server svr;
		std::thread t;
	};

	class RouterAuthTest : public ::testing::Test
	{
	protected:
		MockUpstream upstream; // plays both Kairos and Hephaestus
		Config cfg;
		BroadcasterManager broadcasters{"http://unused.invalid", 30};
		KairosClient kairos{"http://unused.invalid"}; // unused by authedHephaestusProxy itself (it builds its own httplib::Client from cfg)
		LogBuffer logs;
		LogBuffer local_log;
		DeviceSessionManager devices;
		WatchTogetherManager watch_together;

		httplib::Server svr;
		std::unique_ptr<httplib::Client> cli;
		std::thread server_thread;

		void SetUp() override
		{
			upstream.start();
			cfg.kairos_url     = upstream.url;
			cfg.hephaestus_url = upstream.url;

			registerRoutes(svr, broadcasters, kairos, logs, local_log, cfg, devices, watch_together);

			int port      = svr.bind_to_any_port("127.0.0.1");
			server_thread = std::thread([this] { svr.listen_after_bind(); });
			for (int i = 0; i < 200 && !svr.is_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));

			cli = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(port));
			cli->set_connection_timeout(5);
			cli->set_read_timeout(5);
		}

		void TearDown() override
		{
			svr.stop();
			if (server_thread.joinable()) server_thread.join();
		}
	};
} // namespace

TEST_F(RouterAuthTest, NoAuthorizationHeaderIs401)
{
	auto r = cli->Post("/stream/vod/stop", "", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(RouterAuthTest, TokenRejectedByKairosIs401)
{
	upstream.auth_me_response = {401, json{{"error", "invalid token"}}};
	httplib::Headers headers{{"Authorization", "Bearer bad-token"}};

	auto r = cli->Post("/stream/vod/stop", headers, "", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 401);
}

TEST_F(RouterAuthTest, ValidTokenNoAccessCheckNeededProxiesThrough)
{
	// /stop carries no content/channel identity to check, so the access-
	// check branch is skipped entirely -- a valid token alone is enough.
	upstream.auth_me_response = {200, json{{"user_id", "u1"}}};
	httplib::Headers headers{{"Authorization", "Bearer good-token"}};

	auto r = cli->Post("/stream/vod/stop", headers, "", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_EQ(r->body, "proxied-ok");
}

TEST_F(RouterAuthTest, VodStartDeniedByAccessCheckIs403)
{
	upstream.auth_me_response      = {200, json{{"user_id", "u1"}}};
	upstream.access_check_response = {200, json{{"allowed", false}}};
	httplib::Headers headers{{"Authorization", "Bearer good-token"}};
	json body = {{"content_type", "movie"}, {"content_id", "m1"}};

	auto r = cli->Post("/stream/vod/start", headers, body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(RouterAuthTest, VodStartAllowedByAccessCheckProxiesThrough)
{
	upstream.auth_me_response      = {200, json{{"user_id", "u1"}}};
	upstream.access_check_response = {200, json{{"allowed", true}}};
	httplib::Headers headers{{"Authorization", "Bearer good-token"}};
	json body = {{"content_type", "movie"}, {"content_id", "m1"}};

	auto r = cli->Post("/stream/vod/start", headers, body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_EQ(r->body, "proxied-ok");
}

TEST_F(RouterAuthTest, PreviewSwitchDeniedByAccessCheckIs403)
{
	// The other access-check branch (channel_id, not content_type/content_id)
	// -- a separate code path from the VOD case above, real branch coverage
	// not just a cosmetic input variation.
	upstream.auth_me_response      = {200, json{{"user_id", "u1"}}};
	upstream.access_check_response = {200, json{{"allowed", false}}};
	httplib::Headers headers{{"Authorization", "Bearer good-token"}};
	json body = {{"channel_id", "c1"}};

	auto r = cli->Post("/stream/preview/switch", headers, body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(RouterAuthTest, AccessCheckUnreachableFailsOpen)
{
	// access_check_response left unset -- MockUpstream answers with its own
	// 404 for that path, exactly what an unreachable/misconfigured access-
	// check endpoint looks like from the caller's side. Per the documented
	// policy, an infrastructure failure here must never block playback the
	// auth check itself already allowed.
	upstream.auth_me_response = {200, json{{"user_id", "u1"}}};
	httplib::Headers headers{{"Authorization", "Bearer good-token"}};
	json body = {{"content_type", "movie"}, {"content_id", "m1"}};

	auto r = cli->Post("/stream/vod/start", headers, body.dump(), "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
	EXPECT_EQ(r->body, "proxied-ok");
}

TEST_F(RouterAuthTest, MalformedStartBodyFailsOpenRatherThanCrashing)
{
	upstream.auth_me_response = {200, json{{"user_id", "u1"}}};
	httplib::Headers headers{{"Authorization", "Bearer good-token"}};

	auto r = cli->Post("/stream/vod/start", headers, "not valid json", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200) << "malformed body must fail open (caught by the try/catch), not 500 or hang";
	EXPECT_EQ(r->body, "proxied-ok");
}