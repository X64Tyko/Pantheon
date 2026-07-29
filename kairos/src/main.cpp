#include <httplib.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include "api/Router.h"
#include "auth/AuthStore.h"
#include "conf/ConfStore.h"
#include "crash/CrashHandler.h"
#include "db/Database.h"
#include "db/PlaybackHistoryRepository.h"
#include "download/DownloadManager.h"
#include "conf/GuestSettings.h"
#include "email/EmailService.h"
#include "jobs/JobScheduler.h"
#include "log/LogBuffer.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuntimeFlags.h"
#include "scheduler/RuleEngine.h"
#include "source/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>

int main(int argc, char* argv[])
{
	// Force line-buffered output before LogTee so Docker pipe writes flush on
	// every newline at the C stdio level (belt-and-suspenders alongside
	// LogTee::sync() which covers the C++ stream layer).
	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IOLBF, 0);

	// Intercept cout/cerr before anything else so startup messages are captured.
	LogBuffer log_buffer;
	LogTee tee_cout(std::cout, log_buffer);
	LogTee tee_cerr(std::cerr, log_buffer);
	log_buffer.setFile("./data/kairos.log");
	log_buffer.setFilter(kairosLogFilter);
	installCrashHandlers("kairos", "./data", &log_buffer);

	int port              = 8080;
	std::string db_path   = "./data/kairos.db";
	std::string conf_path = "./kairos.conf";
	std::string reset_user, reset_pass;

	for (int i = 1; i < argc; ++i)
	{
		std::string arg = argv[i];
		if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
		else if (arg == "--db" && i + 1 < argc) db_path = argv[++i];
		else if (arg == "--conf" && i + 1 < argc) conf_path = argv[++i];
		else if (arg == "--reset-password" && i + 2 < argc)
		{
			reset_user = argv[++i];
			reset_pass = argv[++i];
		}
		else if (arg == "--debug") g_debug_logging.store(true);
	}

	// Admin recovery mode — reset password and exit without starting the server.
	if (!reset_user.empty())
	{
		Database db(db_path);
		AuthStore auth(db);
		if (!auth.hasAnyUser())
		{
			std::cerr << "[kairos] no users found — run without --reset-password to set up\n";
			return 1;
		}
		auto users = auth.listUsers();
		std::string target_id;
		for (const auto& u : users)
		{
			if (u.username == reset_user)
			{
				target_id = u.user_id;
				break;
			}
		}
		if (target_id.empty())
		{
			std::cerr << "[kairos] user '" << reset_user << "' not found\n";
			return 1;
		}
		if (!auth.updateUser(target_id, reset_pass, ""))
		{
			std::cerr << "[kairos] failed to update password\n";
			return 1;
		}
		std::cout << "[kairos] password reset for user '" << reset_user << "'\n";
		return 0;
	}

	Database db(db_path);
	ConfStore conf(conf_path);
	SyncManager sync(db, conf);
	// A previously-saved Settings-page override takes priority over
	// KAIROS_SYNC_THREADS (which SyncManager only ever falls back to when
	// no override is set) — otherwise every restart silently reverts to
	// whatever the compose file/env sets, discarding what was configured.
	if (int persisted = conf.getSyncThreadsOverride(); persisted > 0)
	{
		sync.setThreadCount(persisted);
	}
	RuleEngine engine(db);
	EPGMaterializer materializer(db, engine);
	DownloadManager dl;
	AuthStore auth(db);
	EmailService email(db);
	sync.loadSources();

	// Public-demo-safe admin bootstrap: if the operator supplied credentials
	// via the environment (see docker-compose.yml's KAIROS_ADMIN_USERNAME/
	// KAIROS_ADMIN_PASSWORD), create the admin account here, before the HTTP
	// port ever opens. Without this, POST /api/auth/setup lets whoever reaches
	// the server first — not necessarily the operator — claim the admin
	// account; on a publicly reachable demo that could be any visitor who
	// beats the operator to it. Bootstrapping here closes that window: by the
	// time svr.listen() runs below, hasAnyUser() is already true, so
	// /api/auth/setup simply reports "already complete" for anyone else.
	if (!auth.hasAnyUser())
	{
		const char* env_user = std::getenv("KAIROS_ADMIN_USERNAME");
		const char* env_pass = std::getenv("KAIROS_ADMIN_PASSWORD");
		if (env_user && env_pass && *env_user && *env_pass)
		{
			if (auth.createUser(env_user, env_pass, "admin")) std::cout << "[kairos] admin account '" << env_user << "' created from KAIROS_ADMIN_USERNAME/KAIROS_ADMIN_PASSWORD\n";
			else std::cerr << "[kairos] KAIROS_ADMIN_USERNAME/KAIROS_ADMIN_PASSWORD set but admin creation failed (username taken?)\n";
		}
	}

	// Load persisted runtime flags from app_config.
	// Env vars and --debug can force flags on; if not forced, use DB value.
	auto loadFlag = [&](const char* key, std::atomic<bool>& flag)
	{
		if (flag.load()) return; // already forced on by env var or CLI arg
		SQLite::Statement q(db.get(), "SELECT value FROM app_config WHERE key=?");
		q.bind(1, std::string(key));
		if (q.executeStep() && q.getColumn(0).getString() == "1") flag.store(true);
	};
	loadFlag("sync_debug", g_debug_logging);
	loadFlag("epg_debug", g_epg_debug);
	loadFlag("verbose_transcode_logs", g_verbose_transcode_logs);
	loadFlag("verbose_gateway_logs", g_verbose_gateway_logs);
	loadFlag("hades_debug", g_hades_debug);

	// KAIROS_API_THREADS lets operators reserve HTTP capacity independently of
	// the sync worker pool (KAIROS_SYNC_THREADS). Lower sync threads in Docker
	// when running against large libraries to prevent starving HTTP requests.
	int api_threads = 8;
	if (const char* env = std::getenv("KAIROS_API_THREADS"))
	{
		try
		{
			int n = std::stoi(env);
			if (n > 0) api_threads = n;
		}
		catch (...) { std::cerr << "[kairos] invalid KAIROS_API_THREADS — using 8\n"; }
	}

	httplib::Server svr;
	svr.new_task_queue = [api_threads] { return new httplib::ThreadPool(api_threads); };
	// httplib defaults this to SIZE_MAX (unbounded) — every request body in
	// this API is a JSON config/metadata payload (no file-upload endpoints
	// exist), so a generous-but-bounded cap blocks a trivial memory/disk
	// exhaustion vector (one huge POST body) without risking any legitimate
	// request. Read/write timeouts are left at httplib's own 5s default,
	// which is already reasonable and unrelated to this.
	svr.set_payload_max_length(25 * 1024 * 1024);

	svr.Get("/health", [](const httplib::Request&, httplib::Response& res)
	{
		res.set_content(R"({"status":"ok","service":"kairos"})", "application/json");
	});

	Router router(svr, db, sync, conf, log_buffer, engine, materializer, dl, auth, email);
	router.registerRoutes();

	if (g_debug_logging.load()) std::cout << "[kairos] sync debug logging enabled\n";
	if (g_epg_debug.load()) std::cout << "[kairos] EPG debug logging enabled\n";
	std::cout << "[kairos] api_threads=" << api_threads
		<< "  sync_threads=" << sync.getThreadCount() << std::endl;
	std::cout << "[kairos] listening on 0.0.0.0:" << port
		<< "  db=" << db_path
		<< "  conf=" << conf_path << std::endl;

	// Periodic maintenance jobs — see JobScheduler.h's own comment for why
	// this is a separate mechanism from the coordinator thread below (which
	// stays focused on sync-coordination concerns tightly coupled to its own
	// pause/resume protocol) and from kairos/src/scheduler/ (EPG/channel
	// scheduling, an unrelated meaning of "scheduler").
	JobScheduler jobs;
	jobs.registerInterval("playback_history_prune", std::chrono::hours(24), [&db]
	{
		// Prune play-history rows older than 180 days so a long-running
		// server doesn't grow this table forever (see migration v91's own
		// comment).
		constexpr int64_t kHistoryMaxAgeMs = 180LL * 24 * 60 * 60 * 1000;
		auto now_ms                        = static_cast<int64_t>(std::time(nullptr)) * 1000;
		int pruned                         = PlaybackHistoryRepository(db).prune(kHistoryMaxAgeMs, now_ms);
		if (pruned > 0) std::cout << "[kairos] pruned " << pruned << " old play-history row(s)" << std::endl;
	});
	// Hourly is plenty of margin for the default 7-day idle window, and lets
	// an admin who dials guest_idle_timeout_days down for testing actually
	// see a guest get pruned within a reasonable wait rather than up to a
	// full day late.
	jobs.registerInterval("guest_prune", std::chrono::hours(1), [&db, &auth]
	{
		int pruned = auth.pruneIdleGuests(static_cast<int64_t>(guest_settings::idleTimeoutDays(db)) * 24 * 3600);
		if (pruned > 0) std::cout << "[kairos] pruned " << pruned << " idle guest account(s)" << std::endl;
	});
	jobs.start();

	// Coordinator thread: keeps Hades responsive while sync runs.
	//
	// Responsibilities:
	//  1. Flush stdout/stderr every 500 ms so Docker pipe buffering never
	//     silences log output between sync passes.
	//  2. Every 5 s during an active sync, signal the sync thread to pause at
	//     its next transaction boundary for 100 ms.  HTTP write handlers
	//     (channel creation, playlist edits, etc.) use this window to grab the
	//     SQLite write lock without competing with a sync transaction.
	//  3. Emit a heartbeat every 30 s while sync is running so the log stream
	//     never goes completely silent during a large library pass.
	std::atomic<bool> coordinator_stop{false};
	std::thread coordinator([&]()
	{
		using namespace std::chrono_literals;
		int ticks = 0;
		while (!coordinator_stop.load(std::memory_order_relaxed))
		{
			std::this_thread::sleep_for(500ms);
			fflush(stdout);
			fflush(stderr);
			++ticks;

			if (!sync.isSyncing()) continue;

			// Every 5 s: open a write window by pausing sync between transactions.
			if (ticks % 10 == 0)
			{
				sync.requestYield();
				std::this_thread::sleep_for(100ms);
				sync.clearYield();
			}

			// Heartbeat every 30 s so the log never goes silent mid-sync.
			if (ticks % 60 == 0)
			{
				std::cout << "[kairos] sync in progress..." << std::endl;
			}
		}
	});

	// Run the HTTP server on the main thread; the coordinator runs alongside it.
	svr.listen("0.0.0.0", port);

	coordinator_stop.store(true);
	coordinator.join();
	jobs.stop();
}
