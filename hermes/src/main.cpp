#include "Config.h"
#include "api/Router.h"
#include "broadcast/BroadcasterManager.h"
#include "devices/DeviceSessionManager.h"
#include "kairos/KairosClient.h"
#include "log/LogBuffer.h"
#include "log/RuntimeFlags.h"
#include <httplib.h>
#include <iostream>
#include <thread>
#include <chrono>

// Subscribe to an upstream SSE log stream and push lines into LogBuffer.
// Reconnects indefinitely on disconnect. Intended to be run on a detached thread.
static void relayUpstreamLogs(const std::string& upstream_url, LogBuffer& dest) {
    while (true) {
        httplib::Client cli(upstream_url);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(60);

        std::string partial;
        cli.Get("/api/logs/stream",
            [](const httplib::Response&) -> bool { return true; },
            [&dest, &partial](const char* data, size_t len) -> bool {
                for (size_t i = 0; i < len; ++i) {
                    char c = data[i];
                    if (c == '\n') {
                        // SSE line format: "data:payload" — extract payload.
                        if (partial.size() > 5 && partial.substr(0, 5) == "data:") {
                            dest.push(partial.substr(5));
                        }
                        partial.clear();
                    } else if (c != '\r') {
                        partial += c;
                    }
                }
                return true;
            });

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

int main(int argc, char* argv[]) {
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
    local_log.setFile("./data/hermes.log");
    local_log.setForward(&combined_log);
    LogTee    tee_cout(std::cout, local_log);
    LogTee    tee_cerr(std::cerr, local_log);

    Config cfg = parseConfig(argc, argv);

    KairosClient kairos(cfg.kairos_url);
    BroadcasterManager broadcasters(cfg.hephaestus_url, cfg.linger_secs);
    DeviceSessionManager devices;

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(32); };

	// Log any 4XX/5XX response so we don't have to rely on client-side errors
	// to discover Hermes returning unexpected status codes.
	  svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
	  if (res.status >= 400) {
	  // Every Roku polls its own commands/next long-poll continuously from
	  // launch, whether or not anything is casting to it — a 404 there just
	  // means "no caster session right now," the routine common case, not
	  // an actual error. Logging it drowned out everything else at one
	  // entry every couple of seconds per idle device (DeviceLongPollTask.brs).
	  bool routineDeviceNotConnected = res.status == 404 &&
	      req.path.find("/commands/next") != std::string::npos;
	  if (!routineDeviceNotConnected) {
	  std::cerr << "[hermes] " << req.method << " " << req.path
	       << " → " << res.status << "\n";
	  }
		}
	});
	
    registerRoutes(svr, broadcasters, kairos, combined_log, cfg, devices);

    // Relay upstream log streams so the Hades UI sees all service logs via
    // a single /api/logs/stream endpoint on Hermes.
    std::thread([&combined_log, url = cfg.kairos_url] {
        relayUpstreamLogs(url, combined_log);
    }).detach();
    std::thread([&combined_log, url = cfg.hephaestus_url] {
        relayUpstreamLogs(url, combined_log);
    }).detach();

    // Keep g_verbose_gateway_logs fresh from Kairos's persisted setting —
    // same polling idea as Hephaestus's SessionManager::refreshCache() for
    // its own verbose flag, just not worth a whole cache class here for one
    // bool. Gates local_log's [hermes]/[roku-ecp] push-to-Hades filtering;
    // hermes.log itself always gets everything regardless.
    std::thread([&kairos, &svr] {
        while (svr.is_running()) {
            if (auto v = kairos.getVerboseGatewayLogs())
                g_verbose_gateway_logs.store(*v, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::seconds(15));
        }
    }).detach();

    // Periodic reap of dead broadcasters (every 60s).
    std::thread reaper([&broadcasters, &svr] {
        while (svr.is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            broadcasters.reap();
        }
    });
    reaper.detach();

    // Periodic reap of device sessions whose long-poll/state-post hasn't
    // reconnected in 45s — comfortably past the 25s long-poll timeout plus
    // a connection hiccup, so a healthy channel is never reaped mid-cycle.
    std::thread device_reaper([&devices, &svr] {
        while (svr.is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            devices.reap(45'000);
        }
    });
    device_reaper.detach();

    std::cout << "[hermes] listening on :" << cfg.port
              << "  hephaestus=" << cfg.hephaestus_url
              << "  kairos=" << cfg.kairos_url << "\n";

    svr.listen("0.0.0.0", cfg.port);
    return 0;
}
