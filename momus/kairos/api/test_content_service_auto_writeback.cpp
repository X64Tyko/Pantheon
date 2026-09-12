// Tests for ContentService::autoWritebackIfEnabled and the per-target
// writeback shaping (art/external_ids/collections opt-outs) it and the
// manual/bulk writeback paths share. Constructs ContentService directly
// (ServiceContext is a plain aggregate of references — see
// api/ServiceContext.h) rather than going through a full Router+HTTP
// fixture: lighter, and this is testing ContentService's own logic, not
// route wiring (that's covered by api/test_writeback_all.cpp's auth-gating
// tests). Real mock Jellyfin-shaped TestServers stand in for the two
// sources so the whole chain — SourceRepository's per-target flags →
// ContentService's filtering/shaping → JellyfinBaseSource::pushMetadata's
// actual HTTP call — is exercised for real, not just inspected.

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

#include "api/ServiceContext.h"
#include "api/services/ContentService.h"
#include "auth/AuthStore.h"
#include "conf/ConfStore.h"
#include "db/ContentRepository.h"
#include "db/Database.h"
#include "db/SourceRepository.h"
#include "download/DownloadManager.h"
#include "email/EmailService.h"
#include "log/LogBuffer.h"
#include "scheduler/EPGDivergenceChecker.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "scraper/ScraperManager.h"
#include "source/SyncManager.h"
#include "api/ScheduleCache.h"
#include "util/RateLimiter.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
	std::string randomSuffix()
	{
		std::random_device rd;
		std::mt19937_64 gen(rd());
		std::uniform_int_distribution<uint64_t> dist;
		char buf[17];
		std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(dist(gen)));
		return buf;
	}

	struct MockJellyfin
	{
		httplib::Server svr;
		int port{0};
		std::thread thread;
		json captured_body;
		bool got_request = false;

		MockJellyfin()
		{
			port = svr.bind_to_any_port("127.0.0.1");
			svr.Get("/Users//Items/item1", [this](const httplib::Request&, httplib::Response& res)
			{
				res.set_content(json{{"Id", "item1"}, {"Name", "Old Title"}}.dump(), "application/json");
			});
			svr.Post("/Items/item1", [this](const httplib::Request& req, httplib::Response& res)
			{
				got_request   = true;
				captured_body = json::parse(req.body);
				res.status    = 200;
				res.set_content("{}", "application/json");
			});
		}

		void start()
		{
			thread = std::thread([this] { svr.listen_after_bind(); });
			while (!svr.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		~MockJellyfin()
		{
			svr.stop();
			if (thread.joinable()) thread.join();
		}

		std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
	};
} // namespace

class ContentServiceAutoWritebackTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	std::string conf_path = (fs::temp_directory_path() / ("momus_awb_conf_" + randomSuffix())).string();
	ConfStore conf{conf_path};
	SyncManager sync{db, conf};
	ScheduleCache schedule_cache{db};
	RuleEngine engine{db};
	EPGMaterializer materializer{db, engine};
	EPGDivergenceChecker divergence_checker{materializer};
	AuthStore auth{db};
	LogBuffer logs;
	DownloadManager dl;
	EmailService email{db};
	ScraperManager scraper{db, conf};
	RateLimiter guest_mutation_limiter;

	ServiceContext ctx{
		db, conf, sync, schedule_cache, materializer, divergence_checker,
		engine, auth, logs, dl, email, guest_mutation_limiter
	};
	ContentService content{ctx, scraper};

	void TearDown() override
	{
		std::error_code ec;
		fs::remove(conf_path, ec);
	}

	void insertMediaSource(const std::string& id, const std::string& base_url,
						   bool auto_writeback, bool update_art, bool update_ext_ids, bool update_collections)
	{
		SQLite::Statement s(db.get(), R"(
            INSERT INTO media_source (source_id, source_type, display_name, base_url,
                auto_writeback, writeback_update_art, writeback_update_external_ids, writeback_update_collections)
            VALUES (?,'jellyfin',?,?,?,?,?,?)
        )");
		s.bind(1, id);
		s.bind(2, id + " display");
		s.bind(3, base_url);
		s.bind(4, auto_writeback ? 1 : 0);
		s.bind(5, update_art ? 1 : 0);
		s.bind(6, update_ext_ids ? 1 : 0);
		s.bind(7, update_collections ? 1 : 0);
		s.exec();
	}

	void mapShow(const std::string& source_id, const std::string& show_id)
	{
		SQLite::Statement s(db.get(), R"(
            INSERT INTO source_mapping (item_type, kairos_id, source_id, external_id)
            VALUES ('show', ?, ?, 'item1')
        )");
		s.bind(1, show_id);
		s.bind(2, source_id);
		s.exec();
	}

	void insertShow(const std::string& id, bool match_confirmed)
	{
		SQLite::Statement s(db.get(), R"(
            INSERT INTO show (show_id, title, genres, collections, imdb_id, match_confirmed)
            VALUES (?, 'Test Show', '["Drama"]', '["Test Collection"]', 'tt123', ?)
        )");
		s.bind(1, id);
		s.bind(2, match_confirmed ? 1 : 0);
		s.exec();
	}
};

TEST_F(ContentServiceAutoWritebackTest, PushesOnlyToSourceWithAutoWritebackEnabled)
{
	MockJellyfin on_server, off_server;
	on_server.start();
	off_server.start();

	insertMediaSource("src-on", on_server.url(), /*auto*/true, true, true, true);
	insertMediaSource("src-off", off_server.url(), /*auto*/false, true, true, true);
	insertShow("show1", /*match_confirmed*/true);
	mapShow("src-on", "show1");
	mapShow("src-off", "show1");
	sync.loadSources();

	content.autoWritebackIfEnabled("show", "show1");

	EXPECT_TRUE(on_server.got_request);
	EXPECT_FALSE(off_server.got_request);
}

TEST_F(ContentServiceAutoWritebackTest, DoesNothingWhenMatchNotConfirmed)
{
	MockJellyfin on_server;
	on_server.start();

	insertMediaSource("src-on", on_server.url(), /*auto*/true, true, true, true);
	insertShow("show1", /*match_confirmed*/false);
	mapShow("src-on", "show1");
	sync.loadSources();

	content.autoWritebackIfEnabled("show", "show1");

	EXPECT_FALSE(on_server.got_request);
}

TEST_F(ContentServiceAutoWritebackTest, DoesNothingForNonexistentItem)
{
	// No crash, no request — just a clean no-op.
	content.autoWritebackIfEnabled("show", "does-not-exist");
	SUCCEED();
}

TEST_F(ContentServiceAutoWritebackTest, RespectsPerTargetExternalIdAndCollectionToggles)
{
	MockJellyfin full_server, restricted_server;
	full_server.start();
	restricted_server.start();

	insertMediaSource("src-full", full_server.url(), true, true, /*ext_ids*/true, /*collections*/true);
	insertMediaSource("src-restricted", restricted_server.url(), true, true, /*ext_ids*/false, /*collections*/false);
	insertShow("show1", true);
	mapShow("src-full", "show1");
	mapShow("src-restricted", "show1");
	sync.loadSources();

	content.autoWritebackIfEnabled("show", "show1");

	ASSERT_TRUE(full_server.got_request);
	EXPECT_TRUE(full_server.captured_body.contains("ProviderIds"));

	ASSERT_TRUE(restricted_server.got_request);
	EXPECT_FALSE(restricted_server.captured_body.contains("ProviderIds"));
	// Genres (not toggle-gated) still goes through on the restricted target —
	// confirms the toggle is scoped to exactly external_ids/collections/art,
	// not "skip this target entirely."
	EXPECT_TRUE(restricted_server.captured_body.contains("Genres"));
}