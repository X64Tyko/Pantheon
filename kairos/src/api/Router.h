#pragma once
#include <httplib.h>

class ConfStore;
class Database;
class EPGMaterializer;
class LogBuffer;
class RuleEngine;
class SyncManager;

class Router {
public:
    Router(httplib::Server& svr, Database& db, SyncManager& sync,
           ConfStore& conf, LogBuffer& logs,
           RuleEngine& engine, EPGMaterializer& materializer);
    void registerRoutes();

private:
    void registerSourceRoutes();
    void registerConfigRoutes();
    void registerChannelRoutes();
    void registerBlockRoutes();
    void registerContentRoutes();
    void registerPlaylistRoutes();
    void registerFillerRoutes();
    void registerActivityRoutes();
    void registerSchedulerRoutes();

    static void ok(httplib::Response& res, const std::string& json);
    static void err(httplib::Response& res, int status, const std::string& msg);

    void proxyImage(const std::string& imgPath, const std::string& sourceId,
                    httplib::Response& res);

    // Fetch items from a Plex playlist/collection, replace list items, record link.
    void syncPlexListItems(httplib::Response& res,
                           const std::string& list_type,
                           const std::string& list_id,
                           const std::string& source_id,
                           const std::string& external_id,
                           const std::string& plex_type);

    httplib::Server&  svr_;
    Database&         db_;
    SyncManager&      sync_;
    ConfStore&        conf_;
    LogBuffer&        logs_;
    RuleEngine&       engine_;
    EPGMaterializer&  materializer_;
};
