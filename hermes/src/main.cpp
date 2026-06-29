#include "Config.h"
#include "api/Router.h"
#include "broadcast/BroadcasterManager.h"
#include "kairos/KairosClient.h"
#include <httplib.h>
#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char* argv[]) {
    Config cfg = parseConfig(argc, argv);

    KairosClient kairos(cfg.kairos_url);
    BroadcasterManager broadcasters(cfg.hephaestus_url, cfg.linger_secs);

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(32); };

    registerRoutes(svr, broadcasters, kairos, cfg);

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
