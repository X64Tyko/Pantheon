#include "Config.h"
#include "api/Router.h"
#include "broadcast/BroadcasterManager.h"
#include "kairos/KairosClient.h"
#include "log/LogBuffer.h"
#include <httplib.h>
#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char* argv[]) {
    LogBuffer log_buffer;
    LogTee    tee_cout(std::cout, log_buffer);
    LogTee    tee_cerr(std::cerr, log_buffer);

    Config cfg = parseConfig(argc, argv);

    KairosClient kairos(cfg.kairos_url);
    BroadcasterManager broadcasters(cfg.hephaestus_url, cfg.linger_secs);

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(32); };

    registerRoutes(svr, broadcasters, kairos, log_buffer, cfg);

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
