#pragma once
#include <httplib.h>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>

class ConfStore;
class Database;
class DownloadManager;
class EPGMaterializer;
class LogBuffer;
class RuleEngine;
class SyncManager;

class Router {
public:
    Router(httplib::Server& svr, Database& db, SyncManager& sync,
           ConfStore& conf, LogBuffer& logs,
           RuleEngine& engine, EPGMaterializer& materializer,
           DownloadManager& dl);
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
    void registerDownloadRoutes();

    static void ok(httplib::Response& res, const std::string& json);
    static void err(httplib::Response& res, int status, const std::string& msg);

    void proxyImage(const std::string& imgPath, const std::string& sourceId,
                    httplib::Response& res);

    // Delete all cached schedule rows for a channel so the next EPG request
    // rebuilds from the current block configuration.
    void clearScheduleCache(const std::string& channel_id);

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
    DownloadManager&  dl_;

    // Preview simulation cache: keyed by channel_id, valid for one (seed, week_anchor) pair.
    // Populated by the POST /epg/preview endpoint when seed is set and no draft blocks are
    // present; invalidated by clearScheduleCache whenever blocks or content change.
    struct PreviewCacheEntry {
        int         seed;
        std::time_t week_anchor;
        std::string body;
    };
    std::mutex                                         preview_mu_;
    std::unordered_map<std::string, PreviewCacheEntry> preview_cache_;
};
