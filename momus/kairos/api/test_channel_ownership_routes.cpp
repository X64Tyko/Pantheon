// Coverage for the self-service channel-builder ownership model (see
// CHANGELOG): channel.owner_user_id/is_demo, channel_auth::canEditChannel,
// the guest-vs-real-viewer branching in POST /api/channels, the
// GET /api/channels visibility filter, and the /playlist.m3u + /epg.xml
// demo-channel exclusion. Also locks in the TimeslotService fix -- those
// routes previously had NO role/ownership check at all.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

#include "api/Router.h"
#include "auth/AuthStore.h"
#include "conf/ConfStore.h"
#include "db/ConfigRepository.h"
#include "db/Database.h"
#include "download/DownloadManager.h"
#include "email/EmailService.h"
#include "log/LogBuffer.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "source/SyncManager.h"

using json = nlohmann::json;

class ChannelOwnershipRoutesTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	ConfStore conf{"./momus_channel_ownership_test.conf"};
	SyncManager sync{db, conf};
	RuleEngine engine{db};
	EPGMaterializer materializer{db, engine};
	DownloadManager dl;
	AuthStore auth{db};
	EmailService email{db};
	LogBuffer logs;

	httplib::Server svr;
	std::unique_ptr<Router> router;
	std::unique_ptr<httplib::Client> cli;
	std::thread server_thread;

	std::string admin_id, admin_token;
	std::string guest1_id, guest1_token;
	std::string guest2_id, guest2_token;
	std::string viewer_id, viewer_token; // real named account, no grant yet

	void SetUp() override
	{
		router = std::make_unique<Router>(svr, db, sync, conf, logs, engine, materializer, dl, auth, email);
		router->registerRoutes();

		int port      = svr.bind_to_any_port("127.0.0.1");
		server_thread = std::thread([this] { svr.listen_after_bind(); });
		for (int i = 0; i < 200 && !svr.is_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));

		cli = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(port));
		cli->set_connection_timeout(5);
		cli->set_read_timeout(5);

		ConfigRepository(db).setValue("guest_profiles_enabled", "1");
		ConfigRepository(db).setValue("guest_channel_builder_enabled", "1");

		auth.createUser("co_admin", "co-password-1", "admin");
		for (const auto& u : auth.listUsers()) if (u.username == "co_admin") admin_id = u.user_id;
		admin_token = auth.login("co_admin", "co-password-1");

		std::tie(guest1_id, guest1_token) = auth.createGuestUser("Guest One");
		std::tie(guest2_id, guest2_token) = auth.createGuestUser("Guest Two");

		auth.createUser("co_viewer", "co-password-1", "viewer");
		for (const auto& u : auth.listUsers()) if (u.username == "co_viewer") viewer_id = u.user_id;
		viewer_token = auth.login("co_viewer", "co-password-1");
	}

	void TearDown() override
	{
		svr.stop();
		if (server_thread.joinable()) server_thread.join();
		std::error_code ec;
		std::filesystem::remove("./momus_channel_ownership_test.conf", ec);
	}

	httplib::Headers headers(const std::string& token) const { return {{"Authorization", "Bearer " + token}}; }
};

// ---------------------------------------------------------------------------
// POST /api/channels -- creation gating + quota
// ---------------------------------------------------------------------------

TEST_F(ChannelOwnershipRoutesTest, GuestCreatesDemoChannelSuccessfully)
{
	auto r = cli->Post("/api/channels", headers(guest1_token),
					   R"({"name":"Demo","number":900})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 201);
}

TEST_F(ChannelOwnershipRoutesTest, GuestCreationRefusedWhenChannelBuilderDisabled)
{
	ConfigRepository(db).setValue("guest_channel_builder_enabled", "0");
	auto r = cli->Post("/api/channels", headers(guest1_token),
					   R"({"name":"Demo","number":900})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(ChannelOwnershipRoutesTest, GuestHitsDemoChannelCap)
{
	ConfigRepository(db).setValue("guest_max_demo_channels", "1");
	auto r1 = cli->Post("/api/channels", headers(guest1_token),
						R"({"name":"Demo 1","number":901})", "application/json");
	ASSERT_TRUE(r1);
	EXPECT_EQ(r1->status, 201);

	auto r2 = cli->Post("/api/channels", headers(guest1_token),
						R"({"name":"Demo 2","number":902})", "application/json");
	ASSERT_TRUE(r2);
	EXPECT_EQ(r2->status, 403);
}

TEST_F(ChannelOwnershipRoutesTest, RealViewerWithoutGrantRefused)
{
	auto r = cli->Post("/api/channels", headers(viewer_token),
					   R"({"name":"Mine","number":910})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(ChannelOwnershipRoutesTest, RealViewerWithGrantCreatesNonDemoChannel)
{
	auth.updateChannelBuilderEnabled(viewer_id, true);

	auto r = cli->Post("/api/channels", headers(viewer_token),
					   R"({"name":"Mine","number":911})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 201);

	// Confirm via the admin-visible unfiltered list that it landed with
	// owner_user_id set and is_demo=false, unlike a guest's channel.
	auto list = cli->Get("/api/channels", headers(admin_token));
	ASSERT_TRUE(list);
	json body  = json::parse(list->body);
	bool found = false;
	for (auto& c : body)
	{
		if (c["channel_id"] == json::parse(r->body)["channel_id"])
		{
			found = true;
			EXPECT_EQ(c["owner_user_id"], viewer_id);
			EXPECT_EQ(c["is_demo"], false);
		}
	}
	EXPECT_TRUE(found);
}

TEST_F(ChannelOwnershipRoutesTest, RealViewerHitsChannelCap)
{
	auth.updateChannelBuilderEnabled(viewer_id, true);
	ConfigRepository(db).setValue("viewer_max_channels", "1");

	auto r1 = cli->Post("/api/channels", headers(viewer_token),
						R"({"name":"Mine 1","number":912})", "application/json");
	ASSERT_TRUE(r1);
	EXPECT_EQ(r1->status, 201);

	auto r2 = cli->Post("/api/channels", headers(viewer_token),
						R"({"name":"Mine 2","number":913})", "application/json");
	ASSERT_TRUE(r2);
	EXPECT_EQ(r2->status, 403);
}

// ---------------------------------------------------------------------------
// Ownership enforcement on existing channels -- channel + block + timeslot
// ---------------------------------------------------------------------------

TEST_F(ChannelOwnershipRoutesTest, GuestCannotEditAdminChannel)
{
	SQLite::Statement c(db.get(), "INSERT INTO channel (channel_id, name, number) VALUES ('c1','Admin Ch',1)");
	c.exec();

	auto r = cli->Patch("/api/channels/c1", headers(guest1_token), R"({"name":"Hijacked"})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);

	auto d = cli->Delete("/api/channels/c1", headers(guest1_token));
	ASSERT_TRUE(d);
	EXPECT_EQ(d->status, 403);
}

TEST_F(ChannelOwnershipRoutesTest, GuestCannotEditAnotherGuestsChannel)
{
	auto created = cli->Post("/api/channels", headers(guest1_token),
							 R"({"name":"Guest1 Demo","number":920})", "application/json");
	ASSERT_TRUE(created);
	std::string channel_id = json::parse(created->body)["channel_id"];

	auto r = cli->Patch("/api/channels/" + channel_id, headers(guest2_token),
						R"({"name":"Stolen"})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(ChannelOwnershipRoutesTest, GuestCanEditOwnDemoChannel)
{
	auto created = cli->Post("/api/channels", headers(guest1_token),
							 R"({"name":"Guest1 Demo","number":921})", "application/json");
	ASSERT_TRUE(created);
	std::string channel_id = json::parse(created->body)["channel_id"];

	auto r = cli->Patch("/api/channels/" + channel_id, headers(guest1_token),
						R"({"name":"Renamed"})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 200);
}

// TimeslotService previously had zero role/ownership check on any route --
// this both confirms the new gate exists and regression-locks the fix.
TEST_F(ChannelOwnershipRoutesTest, GuestCannotCreateTimeslotOnAnotherUsersBlock)
{
	SQLite::Statement c(db.get(), "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('c1','Ch',1,?,1)");
	c.bind(1, guest2_id);
	c.exec();
	SQLite::Statement b(db.get(),
						"INSERT INTO block (block_id, channel_id, block_type) VALUES ('b1','c1','episode')");
	b.exec();

	auto r = cli->Post("/api/blocks/b1/slots", headers(guest1_token),
					   R"({"slot_duration_mins":30})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

TEST_F(ChannelOwnershipRoutesTest, GuestCanCreateTimeslotOnOwnBlock)
{
	SQLite::Statement c(db.get(), "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('c1','Ch',1,?,1)");
	c.bind(1, guest1_id);
	c.exec();
	SQLite::Statement b(db.get(),
						"INSERT INTO block (block_id, channel_id, block_type) VALUES ('b1','c1','episode')");
	b.exec();

	auto r = cli->Post("/api/blocks/b1/slots", headers(guest1_token),
					   R"({"slot_duration_mins":30})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 201);
}

TEST_F(ChannelOwnershipRoutesTest, GuestCannotAddBlockToAnotherUsersChannel)
{
	SQLite::Statement c(db.get(), "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('c1','Ch',1,?,1)");
	c.bind(1, guest2_id);
	c.exec();

	auto r = cli->Post("/api/channels/c1/blocks", headers(guest1_token),
					   R"({"block_type":"episode","day_mask":127,"start_time":"00:00"})", "application/json");
	ASSERT_TRUE(r);
	EXPECT_EQ(r->status, 403);
}

// ---------------------------------------------------------------------------
// GET /api/channels visibility
// ---------------------------------------------------------------------------

TEST_F(ChannelOwnershipRoutesTest, ChannelListHidesOtherGuestsDemoChannelFromNonAdmin)
{
	SQLite::Statement real(db.get(), "INSERT INTO channel (channel_id, name, number) VALUES ('real','Real',1)");
	real.exec();
	SQLite::Statement demo(db.get(), "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('demo','Guest2 Demo',2,?,1)");
	demo.bind(1, guest2_id);
	demo.exec();

	auto r = cli->Get("/api/channels", headers(guest1_token));
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	ASSERT_EQ(body.size(), 1u);
	EXPECT_EQ(body[0]["channel_id"], "real");
}

TEST_F(ChannelOwnershipRoutesTest, ChannelListShowsOwnDemoChannelToOwningGuest)
{
	SQLite::Statement demo(db.get(), "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('demo','My Demo',2,?,1)");
	demo.bind(1, guest1_id);
	demo.exec();

	auto r = cli->Get("/api/channels", headers(guest1_token));
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	ASSERT_EQ(body.size(), 1u);
	EXPECT_EQ(body[0]["channel_id"], "demo");
}

TEST_F(ChannelOwnershipRoutesTest, ChannelListHidesAllDemoChannelsFromUnauthenticatedCaller)
{
	SQLite::Statement real(db.get(), "INSERT INTO channel (channel_id, name, number) VALUES ('real','Real',1)");
	real.exec();
	SQLite::Statement demo(db.get(), "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('demo','Demo',2,?,1)");
	demo.bind(1, guest1_id);
	demo.exec();

	auto r = cli->Get("/api/channels"); // no Authorization header
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	ASSERT_EQ(body.size(), 1u);
	EXPECT_EQ(body[0]["channel_id"], "real");
}

TEST_F(ChannelOwnershipRoutesTest, ChannelListShowsEverythingUnfilteredToAdmin)
{
	SQLite::Statement real(db.get(), "INSERT INTO channel (channel_id, name, number) VALUES ('real','Real',1)");
	real.exec();
	SQLite::Statement demo1(db.get(), "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('demo1','D1',2,?,1)");
	demo1.bind(1, guest1_id);
	demo1.exec();
	SQLite::Statement demo2(db.get(), "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('demo2','D2',3,?,1)");
	demo2.bind(1, guest2_id);
	demo2.exec();

	auto r = cli->Get("/api/channels", headers(admin_token));
	ASSERT_TRUE(r);
	json body = json::parse(r->body);
	EXPECT_EQ(body.size(), 3u);
}

// ---------------------------------------------------------------------------
// /playlist.m3u + /epg.xml -- demo channels excluded, real-viewer channels included
// ---------------------------------------------------------------------------

TEST_F(ChannelOwnershipRoutesTest, PlaylistM3UExcludesDemoChannelButIncludesRealViewerChannel)
{
	SQLite::Statement real(db.get(), "INSERT INTO channel (channel_id, name, number) VALUES ('real','Real',1)");
	real.exec();
	SQLite::Statement demo(db.get(), "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('demo','Demo',2,?,1)");
	demo.bind(1, guest1_id);
	demo.exec();
	SQLite::Statement viewerCh(db.get(),
							   "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('viewerch','ViewerCh',3,?,0)");
	viewerCh.bind(1, viewer_id);
	viewerCh.exec();

	auto r = cli->Get("/playlist.m3u");
	ASSERT_TRUE(r);
	EXPECT_NE(r->body.find("Real"), std::string::npos);
	EXPECT_NE(r->body.find("ViewerCh"), std::string::npos);
	EXPECT_EQ(r->body.find("Demo"), std::string::npos);
}

TEST_F(ChannelOwnershipRoutesTest, XmltvExcludesDemoChannelButIncludesRealViewerChannel)
{
	SQLite::Statement real(db.get(), "INSERT INTO channel (channel_id, name, number) VALUES ('real','Real',1)");
	real.exec();
	SQLite::Statement demo(db.get(), "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('demo','Demo',2,?,1)");
	demo.bind(1, guest1_id);
	demo.exec();
	SQLite::Statement viewerCh(db.get(),
							   "INSERT INTO channel (channel_id, name, number, owner_user_id, is_demo) VALUES ('viewerch','ViewerCh',3,?,0)");
	viewerCh.bind(1, viewer_id);
	viewerCh.exec();

	auto r = cli->Get("/epg.xml");
	ASSERT_TRUE(r);
	EXPECT_NE(r->body.find("Real"), std::string::npos);
	EXPECT_NE(r->body.find("ViewerCh"), std::string::npos);
	EXPECT_EQ(r->body.find("Demo"), std::string::npos);
}