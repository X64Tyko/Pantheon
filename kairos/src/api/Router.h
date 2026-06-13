#pragma once
#include <httplib.h>

class ConfStore;
class Database;
class LogBuffer;
class SyncManager;

class Router {
public:
    Router(httplib::Server& svr, Database& db, SyncManager& sync,
           ConfStore& conf, LogBuffer& logs);
    void registerRoutes();

private:
    void registerSourceRoutes();
    void registerConfigRoutes();
    void registerChannelRoutes();
    void registerContentRoutes();
    void registerActivityRoutes();

    static void ok(httplib::Response& res, const std::string& json);
    static void err(httplib::Response& res, int status, const std::string& msg);

    void proxyImage(const std::string& imgPath, const std::string& sourceId,
                    httplib::Response& res);

    httplib::Server& svr_;
    Database&        db_;
    SyncManager&     sync_;
    ConfStore&       conf_;
    LogBuffer&       logs_;
};
