#include "Config.h"
#include "api/Router.h"
#include "broadcast/BroadcasterManager.h"
#include "crash/CrashHandler.h"
#include "devices/DeviceSessionManager.h"
#include "kairos/InternalToken.h"
#include "kairos/KairosClient.h"
#include "log/LogBuffer.h"
#include "log/RuntimeFlags.h"
#include "thread/TaskRegistry.h"
#include "watchtogether/WatchTogetherManager.h"
#include <httplib.h>
#include <iostream>
#include <stop_token>
#include <chrono>

// Subscribe to an upstream SSE log stream and push lines into LogBuffer.
// Reconnects indefinitely on disconnect (until `st` is stop-requested).
// Intended to be run via TaskRegistry's stop_token overload. internal_token
// authenticates this server-to-server call — both Kairos's and Hephaestus's
// own /api/logs/stream are internal-auth-gated (see their own Router.cpp),
// since this relay (not a browser) is their only caller; empty omits the
// header and lets the upstream's own check reject it, same as before this
// existed for a server with no internal_token configured yet.
static void relayUpstreamLogs(std::stop_token st, const std::string& upstream_url,
							  const std::string& internal_token, LogBuffer& dest)
{
	httplib::Headers headers;
	if (!internal_token.empty()) headers.emplace("X-Internal-Token", internal_token);

	while (!st.stop_requested())
	{
		httplib::Client cli(upstream_url);
		cli.set_connection_timeout(5);
		cli.set_read_timeout(60);

		std::string partial;
		cli.Get("/api/logs/stream", headers,
				[](const httplib::Response&) -> bool { return true; },
				[&dest, &partial](const char* data, size_t len) -> bool
				{
					for (size_t i = 0; i < len; ++i)
					{
						char c = data[i];
						if (c == '\n')
						{
							// SSE line format: "data:payload" — extract payload.
							if (partial.size() > 5 && partial.substr(0, 5) == "data:")
							{
								dest.push(partial.substr(5));
							}
							partial.clear();
						}
						else if (c != '\r')
						{
							partial += c;
						}
					}
					return true;
				});

		stopTokenSleep(st, std::chrono::seconds(5));
	}
}

int main(int argc, char* argv[])
{
	// Intercept cout/cerr before anything else so startup messages are captured.
	//
	// Two buffers, not one: combined_log is what /api/logs/stream actually
	// serves to Hades — it receives Hermes's own (already category-filtered)
	// lines via local_log's forward hook below, plus Kairos's and
	// Hephaestus's relayed streams (relayUpstreamLogs). local_log is
	// Hermes's own file-backed log — kept separate so relaying someone
	// else's already-logged lines through Hermes doesn't also duplicate them
	// into hermes.log.
	LogBuffer combined_log;
	LogBuffer local_log;
	local_log.setForward(&combined_log);
	LogTee tee_cout(std::cout, local_log);
	LogTee tee_cerr(std::cerr, local_log);
	local_log.setFile("./data/hermes.log");
	local_log.setFilter(hermesLogFilter);
	// combined_log, not local_log — it's what /api/logs/stream actually
	// serves (Hermes's own lines plus the relayed Kairos/Hephaestus
	// streams), more useful context for a crash dump than Hermes's own
	// lines alone.
	installCrashHandlers("hermes", "./data", &combined_log);

	Config cfg = parseConfig(argc, argv);

	KairosClient kairos(cfg.kairos_url);
	BroadcasterManager broadcasters(cfg.hephaestus_url, cfg.linger_secs);
	DeviceSessionManager devices;
	WatchTogetherManager watch_together;

	httplib::Server svr;
	svr.new_task_queue = [] { return new httplib::ThreadPool(32); };
	// See Kairos's main.cpp for why — same reasoning, Hermes has no upload
	// endpoints of its own either (it proxies Kairos's/Hephaestus's JSON
	// bodies verbatim).
	svr.set_payload_max_length(25 * 1024 * 1024);

	// Baseline hardening headers on every response — see Kairos's Router.cpp
	// for why no Content-Security-Policy is included here either (same
	// gstatic.com/Cast-SDK/hls.js-worker/websocket surface, same "wrong is
	// worse than absent on a live public demo" reasoning).
	svr.set_post_routing_handler([](const httplib::Request&, httplib::Response& res)
	{
		res.set_header("X-Content-Type-Options", "nosniff");
		res.set_header("X-Frame-Options", "DENY");
		res.set_header("Referrer-Policy", "same-origin");
		res.set_header("Strict-Transport-Security", "max-age=15552000; includeSubDomains");
	});

	// Log any 4XX/5XX response so we don't have to rely on client-side errors
	// to discover Hermes returning unexpected status codes.
	svr.set_logger([](const httplib::Request& req, const httplib::Response& res)
	{
		if (res.status >= 400)
		{
			// Every Roku polls its own commands/next long-poll continuously from
			// launch, whether or not anything is casting to it — a 404 there just
			// means "no caster session right now," the routine common case, not
			// an actual error. Logging it drowned out everything else at one
			// entry every couple of seconds per idle device (DeviceLongPollTask.brs).
			bool routineDeviceNotConnected = res.status == 404 &&
				req.path.find("/commands/next") != std::string::npos;
			if (!routineDeviceNotConnected)
			{
				std::cerr << "[hermes] " << req.method << " " << req.path
					<< " → " << res.status << "\n";
			}
		}
	});

	registerRoutes(svr, broadcasters, kairos, combined_log, local_log, cfg, devices, watch_together);

	// Relay upstream log streams so the Hades UI sees all service logs via
	// a single /api/logs/stream endpoint on Hermes. Read once at startup —
	// matches how Hephaestus's own KairosClient re-reads kairos.conf per
	// request instead of caching it, but this relay reconnects far less often
	// than a request rate, and internal_token isn't meant to be rotated live.
	std::string internal_token = readKairosInternalToken(cfg.kairos_conf_path);
	TaskRegistry::global().spawn([&combined_log, url = cfg.kairos_url, internal_token](std::stop_token st)
	{
		relayUpstreamLogs(st, url, internal_token, combined_log);
	});
	TaskRegistry::global().spawn([&combined_log, url = cfg.hephaestus_url, internal_token](std::stop_token st)
	{
		relayUpstreamLogs(st, url, internal_token, combined_log);
	});

	// Keep g_verbose_gateway_logs fresh from Kairos's persisted setting —
	// same polling idea as Hephaestus's SessionManager::refreshCache() for
	// its own verbose flag, just not worth a whole cache class here for one
	// bool. Gates local_log's [hermes]/[roku-ecp] push-to-Hades filtering;
	// hermes.log itself always gets everything regardless.
	TaskRegistry::global().spawn([&kairos, &svr](std::stop_token st)
	{
		while (svr.is_running() && !st.stop_requested())
		{
			if (auto v = kairos.getVerboseGatewayLogs()) g_verbose_gateway_logs.store(*v, std::memory_order_relaxed);
			stopTokenSleep(st, std::chrono::seconds(15));
		}
	});

	// Periodic reap of dead broadcasters (every 60s).
	TaskRegistry::global().spawn([&broadcasters, &svr](std::stop_token st)
	{
		while (svr.is_running() && !st.stop_requested())
		{
			stopTokenSleep(st, std::chrono::seconds(60));
			broadcasters.reap();
		}
	});

	// Periodic reap of device sessions whose long-poll/state-post hasn't
	// reconnected in 45s — comfortably past the 25s long-poll timeout plus
	// a connection hiccup, so a healthy channel is never reaped mid-cycle.
	TaskRegistry::global().spawn([&devices, &svr](std::stop_token st)
	{
		while (svr.is_running() && !st.stop_requested())
		{
			stopTokenSleep(st, std::chrono::seconds(30));
			devices.reap(45'000);
		}
	});

	// Watch Together: periodic `sync` tick — see WatchTogetherSession's own
	// comment on why this runs independent of host command/heartbeat
	// traffic (a follower who stalled or just joined needs correcting even
	// when the host hasn't touched anything).
	TaskRegistry::global().spawn([&watch_together, &svr](std::stop_token st)
	{
		while (svr.is_running() && !st.stop_requested())
		{
			stopTokenSleep(st, std::chrono::seconds(4));
			watch_together.tickSync();
		}
	});

	// Watch Together: reap sessions whose host has gone quiet for 45s —
	// same grace window as devices.reap() above, for the same reason (a
	// few missed heartbeats at the plan's own "every few seconds" cadence,
	// not a hair-trigger on one dropped request).
	TaskRegistry::global().spawn([&watch_together, &cfg, &svr](std::stop_token st)
	{
		while (svr.is_running() && !st.stop_requested())
		{
			stopTokenSleep(st, std::chrono::seconds(15));
			watch_together.reap(45'000, cfg);
		}
	});

	std::cout << "[hermes] listening on :" << cfg.port
		<< "  hephaestus=" << cfg.hephaestus_url
		<< "  kairos=" << cfg.kairos_url << "\n";

	svr.listen("0.0.0.0", cfg.port);
	return 0;
}