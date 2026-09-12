#include <gtest/gtest.h>
#include "../../hermes/src/watchtogether/WatchTogetherManager.h"
#include "../../hermes/src/Config.h"
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

using json = nlohmann::json;

// A stand-in for Kairos's GET /api/watch-together/:id (+ POST .../close),
// same "spin up a local httplib server" pattern test_kairos_client.cpp uses
// for KairosClient — WatchTogetherManager::getOrCreate/reap make real
// httplib::Client calls out to cfg.kairos_url, so this is the least invasive
// way to exercise that round trip without touching production code.
class MockKairos
{
public:
	MockKairos()
	{
		svr.Get(R"(/api/watch-together/([^/]+)$)", [this](const httplib::Request& req, httplib::Response& res)
		{
			std::string id = req.matches[1];
			get_calls.fetch_add(1);
			std::lock_guard<std::mutex> lock(mtx);
			auto it = sessions.find(id);
			if (it == sessions.end())
			{
				res.status = 404;
				return;
			}
			res.set_content(it->second.dump(), "application/json");
		});
		svr.Post(R"(/api/watch-together/([^/]+)/close$)", [this](const httplib::Request& req, httplib::Response& res)
		{
			std::string id = req.matches[1];
			close_calls.fetch_add(1);
			std::lock_guard<std::mutex> lock(mtx);
			last_close_auth = req.get_header_value("Authorization");
			closed_ids.insert(id);
			res.set_content("{}", "application/json");
		});
	}

	void start()
	{
		int port = svr.bind_to_any_port("127.0.0.1");
		url      = "http://127.0.0.1:" + std::to_string(port);
		t        = std::thread([this]() { svr.listen_after_bind(); });
	}

	void stop()
	{
		svr.stop();
		if (t.joinable()) t.join();
	}

	void setSession(const std::string& id, const std::string& host_user_id, bool closed = false)
	{
		std::lock_guard<std::mutex> lock(mtx);
		sessions[id] = json{
			{"host_user_id", host_user_id},
			{"closed_at", closed ? json("2026-01-01T00:00:00Z") : json(nullptr)}
		};
	}

	bool wasClosed(const std::string& id)
	{
		std::lock_guard<std::mutex> lock(mtx);
		return closed_ids.count(id) > 0;
	}

	std::string url;
	httplib::Server svr;
	std::thread t;
	std::mutex mtx;
	std::map<std::string, json> sessions;
	std::set<std::string> closed_ids;
	std::string last_close_auth;
	std::atomic<int> get_calls{0};
	std::atomic<int> close_calls{0};
};

namespace
{
	Config configFor(const std::string& kairos_url)
	{
		Config cfg;
		cfg.kairos_url = kairos_url;
		return cfg;
	}
} // namespace

// ---- getOrCreate ----------------------------------------------------------

TEST(WatchTogetherManagerTest, GetOrCreateFetchesFromKairosAndCaches)
{
	MockKairos mock;
	mock.start();
	mock.setSession("wt-1", "host-a");
	auto cfg = configFor(mock.url);

	WatchTogetherManager mgr;
	auto s1 = mgr.getOrCreate("wt-1", cfg, "Bearer tok");
	auto s2 = mgr.getOrCreate("wt-1", cfg, "Bearer tok");

	ASSERT_NE(s1, nullptr);
	EXPECT_EQ(s1, s2); // cached, not re-fetched
	EXPECT_EQ(s1->hostUserId(), "host-a");
	EXPECT_EQ(mock.get_calls.load(), 1); // only the first call hit Kairos

	mock.stop();
}

TEST(WatchTogetherManagerTest, GetOrCreateReturnsNullptrForUnknownSession)
{
	MockKairos mock;
	mock.start();
	auto cfg = configFor(mock.url);

	WatchTogetherManager mgr;
	auto session = mgr.getOrCreate("does-not-exist", cfg, "Bearer tok");

	EXPECT_EQ(session, nullptr);
	mock.stop();
}

TEST(WatchTogetherManagerTest, GetOrCreateReturnsNullptrForClosedSession)
{
	MockKairos mock;
	mock.start();
	mock.setSession("wt-closed", "host-a", /*closed=*/true);
	auto cfg = configFor(mock.url);

	WatchTogetherManager mgr;
	auto session = mgr.getOrCreate("wt-closed", cfg, "Bearer tok");

	EXPECT_EQ(session, nullptr);
	mock.stop();
}

TEST(WatchTogetherManagerTest, GetOrCreateReturnsNullptrForEmptyHostUserId)
{
	MockKairos mock;
	mock.start();
	mock.setSession("wt-nohost", "");
	auto cfg = configFor(mock.url);

	WatchTogetherManager mgr;
	auto session = mgr.getOrCreate("wt-nohost", cfg, "Bearer tok");

	EXPECT_EQ(session, nullptr);
	mock.stop();
}

TEST(WatchTogetherManagerTest, GetOrCreateReturnsNullptrOnMalformedKairosResponse)
{
	httplib::Server svr;
	svr.Get(R"(/api/watch-together/([^/]+)$)", [](const httplib::Request&, httplib::Response& res)
	{
		res.set_content("not json", "application/json");
	});
	int port = svr.bind_to_any_port("127.0.0.1");
	std::thread t([&]() { svr.listen_after_bind(); });

	auto cfg = configFor("http://127.0.0.1:" + std::to_string(port));
	WatchTogetherManager mgr;
	auto session = mgr.getOrCreate("wt-bad", cfg, "Bearer tok");

	EXPECT_EQ(session, nullptr);

	svr.stop();
	t.join();
}

// Simulates a second (and third) viewer joining an already-live session:
// each just calls getOrCreate again with the same session_id, exactly what
// the /join route does. The live position/paused state Hermes tracks must
// NOT be reset just because another viewer showed up.
TEST(WatchTogetherManagerTest, RepeatedGetOrCreateSimulatingMultipleJoinsPreservesLiveState)
{
	MockKairos mock;
	mock.start();
	mock.setSession("wt-1", "host-a");
	auto cfg = configFor(mock.url);

	WatchTogetherManager mgr;
	auto host_view = mgr.getOrCreate("wt-1", cfg, "Bearer host-tok");
	host_view->pushCommand({{"type", "play"}, {"position_ms", 90000}});

	// A follower "joins" — same session_id, different caller token.
	auto follower_view = mgr.getOrCreate("wt-1", cfg, "Bearer follower-tok");
	ASSERT_EQ(host_view, follower_view); // same underlying session object
	EXPECT_FALSE(follower_view->isPaused());
	EXPECT_GE(follower_view->authoritativePositionMs(), 90000);

	// A second follower joining later still sees the live position, not 0.
	auto follower2_view = mgr.getOrCreate("wt-1", cfg, "Bearer follower2-tok");
	EXPECT_GE(follower2_view->authoritativePositionMs(), 90000);

	mock.stop();
}

// ---- reap -------------------------------------------------------------

TEST(WatchTogetherManagerTest, ReapDropsStaleSessionAndKeepsFreshOne)
{
	MockKairos mock;
	mock.start();
	mock.setSession("wt-stale", "host-a");
	mock.setSession("wt-fresh", "host-b");
	auto cfg = configFor(mock.url);

	WatchTogetherManager mgr;
	mgr.getOrCreate("wt-stale", cfg, "Bearer tok"); // idle clock starts now

	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	// Freshly touched right before reaping.
	mgr.getOrCreate("wt-fresh", cfg, "Bearer tok");

	EXPECT_EQ(mock.get_calls.load(), 2);
	mgr.reap(100, cfg);

	// wt-stale should have been evicted: re-requesting it forces a fresh
	// Kairos GET (cache miss) and yields a brand-new session object whose
	// live state is back at defaults (position 0 / paused) — proof the old
	// one, and whatever state it had, is really gone.
	auto restale = mgr.getOrCreate("wt-stale", cfg, "Bearer tok");
	ASSERT_NE(restale, nullptr);
	EXPECT_EQ(restale->authoritativePositionMs(), 0);
	EXPECT_TRUE(restale->isPaused());
	EXPECT_EQ(mock.get_calls.load(), 3); // cache miss re-fetched from Kairos

	// wt-fresh should still be the cached object — no extra Kairos GET.
	auto refresh = mgr.getOrCreate("wt-fresh", cfg, "Bearer tok");
	(void)refresh;
	EXPECT_EQ(mock.get_calls.load(), 3); // unchanged — still cached

	mock.stop();
}

TEST(WatchTogetherManagerTest, ReapClosesOnKairosUsingLastSeenHostToken)
{
	MockKairos mock;
	mock.start();
	mock.setSession("wt-1", "host-a");
	auto cfg = configFor(mock.url);

	WatchTogetherManager mgr;
	auto session = mgr.getOrCreate("wt-1", cfg, "Bearer caller-tok");
	session->setHostAuthHeader("Bearer host-real-tok");

	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	mgr.reap(50, cfg);

	// Give the (out-of-lock) best-effort close POST a moment to land.
	for (int i = 0; i < 20 && mock.close_calls.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));

	EXPECT_TRUE(mock.wasClosed("wt-1"));
	EXPECT_EQ(mock.last_close_auth, "Bearer host-real-tok");

	mock.stop();
}

// A session that never saw a real host command/heartbeat (e.g. a follower
// subscribed before the host's first heartbeat, then everyone vanished) has
// no token to authenticate a close call with — reap must still evict it from
// its own map without attempting (and failing) a Kairos close.
TEST(WatchTogetherManagerTest, ReapDoesNotCallCloseWhenNoHostAuthHeaderEverSet)
{
	MockKairos mock;
	mock.start();
	mock.setSession("wt-1", "host-a");
	auto cfg = configFor(mock.url);

	WatchTogetherManager mgr;
	mgr.getOrCreate("wt-1", cfg, "Bearer caller-tok"); // no setHostAuthHeader call

	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	mgr.reap(50, cfg);

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_EQ(mock.close_calls.load(), 0);
	EXPECT_FALSE(mock.wasClosed("wt-1"));

	mock.stop();
}

// ---- tickSync ----------------------------------------------------------

TEST(WatchTogetherManagerTest, TickSyncBroadcastsToAllLiveSessions)
{
	MockKairos mock;
	mock.start();
	mock.setSession("wt-a", "host-a");
	mock.setSession("wt-b", "host-b");
	auto cfg = configFor(mock.url);

	WatchTogetherManager mgr;
	auto a = mgr.getOrCreate("wt-a", cfg, "Bearer tok");
	auto b = mgr.getOrCreate("wt-b", cfg, "Bearer tok");
	a->pushCommand({{"type", "play"}, {"position_ms", 1000}});

	auto a_seq = a->initialSnapshot().second;
	auto b_seq = b->initialSnapshot().second;

	mgr.tickSync();

	auto [a_events, a_new_seq] = a->waitAfter(a_seq, std::chrono::milliseconds(50));
	auto [b_events, b_new_seq] = b->waitAfter(b_seq, std::chrono::milliseconds(50));

	ASSERT_EQ(a_events.size(), 1u);
	EXPECT_EQ(a_events[0]["type"], "sync");
	ASSERT_EQ(b_events.size(), 1u);
	EXPECT_EQ(b_events[0]["type"], "sync");
	EXPECT_GT(a_new_seq, a_seq);
	EXPECT_GT(b_new_seq, b_seq);

	mock.stop();
}

TEST(WatchTogetherManagerTest, TickSyncWithNoLiveSessionsIsANoop)
{
	WatchTogetherManager mgr;
	// Nothing created — must not crash or throw with an empty session map.
	EXPECT_NO_THROW(mgr.tickSync());
}