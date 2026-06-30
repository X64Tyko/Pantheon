#include "Config.h"
#include "api/Router.h"
#include "broadcast/BroadcasterManager.h"
#include "kairos/KairosClient.h"
#include "log/LogBuffer.h"
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
    LogBuffer log_buffer;
    LogTee    tee_cout(std::cout, log_buffer);
    LogTee    tee_cerr(std::cerr, log_buffer);

    Config cfg = parseConfig(argc, argv);

    KairosClient kairos(cfg.kairos_url);
    BroadcasterManager broadcasters(cfg.hephaestus_url, cfg.linger_secs);

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(32); };

	// Log any 4XX/5XX response so we don't have to rely on client-side errors                      
	// to discover Hermes returning unexpected status codes.                                        
	  svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {                  
	  if (res.status >= 400) {                                                                    
	  std::cerr << "[hermes] " << req.method << " " << req.path                               
	       << " → " << res.status << "\n";                                               
		}                                                                                           
	}); 
	
    registerRoutes(svr, broadcasters, kairos, log_buffer, cfg);

    // Relay upstream log streams so the Hades UI sees all service logs via
    // a single /api/logs/stream endpoint on Hermes.
    std::thread([&log_buffer, url = cfg.kairos_url] {
        relayUpstreamLogs(url, log_buffer);
    }).detach();
    std::thread([&log_buffer, url = cfg.hephaestus_url] {
        relayUpstreamLogs(url, log_buffer);
    }).detach();

    // Periodic reap of dead broadcasters (every 60s).
    std::thread reaper([&broadcasters, &svr] {
        while (svr.is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            broadcasters.reap();
        }
    });
    reaper.detach();

    std::cout << "[hermes] listening on :" << cfg.port
              << "  hephaestus=" << cfg.hephaestus_url
              << "  kairos=" << cfg.kairos_url << "\n";

    svr.listen("0.0.0.0", cfg.port);
    return 0;
}
