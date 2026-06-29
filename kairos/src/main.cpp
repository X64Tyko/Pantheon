#include <httplib.h>
#include <iostream>
#include <string>
#include "api/Router.h"
#include "auth/AuthStore.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include "download/DownloadManager.h"
#include "log/LogBuffer.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuntimeFlags.h"
#include "scheduler/RuleEngine.h"
#include "source/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>

int main(int argc, char* argv[]) {
    // Intercept cout/cerr before anything else so startup messages are captured.
    LogBuffer log_buffer;
    LogTee    tee_cout(std::cout, log_buffer);
    LogTee    tee_cerr(std::cerr, log_buffer);

    int port = 8080;
    std::string db_path    = "./data/kairos.db";
    std::string conf_path  = "./kairos.conf";
    std::string reset_user, reset_pass;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc)
            port = std::stoi(argv[++i]);
        else if (arg == "--db" && i + 1 < argc)
            db_path = argv[++i];
        else if (arg == "--conf" && i + 1 < argc)
            conf_path = argv[++i];
        else if (arg == "--reset-password" && i + 2 < argc) {
            reset_user = argv[++i];
            reset_pass = argv[++i];
        }
        else if (arg == "--debug")
            g_debug_logging.store(true);
    }

    // Admin recovery mode — reset password and exit without starting the server.
    if (!reset_user.empty()) {
        Database  db(db_path);
        AuthStore auth(db);
        if (!auth.hasAnyUser()) {
            std::cerr << "[kairos] no users found — run without --reset-password to set up\n";
            return 1;
        }
        auto users = auth.listUsers();
        std::string target_id;
        for (const auto& u : users) {
            if (u.username == reset_user) { target_id = u.user_id; break; }
        }
        if (target_id.empty()) {
            std::cerr << "[kairos] user '" << reset_user << "' not found\n";
            return 1;
        }
        if (!auth.updateUser(target_id, reset_pass, "")) {
            std::cerr << "[kairos] failed to update password\n";
            return 1;
        }
        std::cout << "[kairos] password reset for user '" << reset_user << "'\n";
        return 0;
    }

    Database         db(db_path);
    ConfStore        conf(conf_path);
    SyncManager      sync(db, conf);
    RuleEngine       engine(db);
    EPGMaterializer  materializer(db, engine);
    DownloadManager  dl;
    AuthStore        auth(db);
    sync.loadSources();

    // Load persisted runtime flags from app_config.
    // Env vars and --debug can force flags on; if not forced, use DB value.
    auto loadFlag = [&](const char* key, std::atomic<bool>& flag) {
        if (flag.load()) return; // already forced on by env var or CLI arg
        SQLite::Statement q(db.get(), "SELECT value FROM app_config WHERE key=?");
        q.bind(1, std::string(key));
        if (q.executeStep() && q.getColumn(0).getString() == "1")
            flag.store(true);
    };
    loadFlag("sync_debug", g_debug_logging);
    loadFlag("epg_debug",  g_epg_debug);

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(8); };

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok","service":"kairos"})", "application/json");
    });

    Router router(svr, db, sync, conf, log_buffer, engine, materializer, dl, auth);
    router.registerRoutes();

    if (g_debug_logging.load()) std::cout << "[kairos] sync debug logging enabled\n";
    if (g_epg_debug.load())     std::cout << "[kairos] EPG debug logging enabled\n";
    std::cout << "[kairos] listening on 0.0.0.0:" << port
              << "  db=" << db_path
              << "  conf=" << conf_path << std::endl;

    svr.listen("0.0.0.0", port);
}
