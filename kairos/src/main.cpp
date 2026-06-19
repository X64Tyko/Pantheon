#include <httplib.h>
#include <iostream>
#include <string>
#include "api/Router.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include "download/DownloadManager.h"
#include "log/LogBuffer.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "sync/SyncManager.h"

int main(int argc, char* argv[]) {
    // Intercept cout/cerr before anything else so startup messages are captured.
    LogBuffer log_buffer;
    LogTee    tee_cout(std::cout, log_buffer);
    LogTee    tee_cerr(std::cerr, log_buffer);

    int port = 8080;
    std::string db_path   = "./data/kairos.db";
    std::string conf_path = "./kairos.conf";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc)
            port = std::stoi(argv[++i]);
        else if (arg == "--db" && i + 1 < argc)
            db_path = argv[++i];
        else if (arg == "--conf" && i + 1 < argc)
            conf_path = argv[++i];
    }

    Database         db(db_path);
    ConfStore        conf(conf_path);
    SyncManager      sync(db, conf);
    RuleEngine       engine(db);
    EPGMaterializer  materializer(db, engine);
    DownloadManager  dl;
    sync.loadSources();

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(8); };

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok","service":"kairos"})", "application/json");
    });

    Router router(svr, db, sync, conf, log_buffer, engine, materializer, dl);
    router.registerRoutes();

    std::cout << "[kairos] listening on 0.0.0.0:" << port
              << "  db=" << db_path
              << "  conf=" << conf_path << std::endl;

    svr.listen("0.0.0.0", port);
}
