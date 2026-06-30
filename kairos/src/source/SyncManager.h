#pragma once
#include "IMediaSource.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Database;
class ConfStore;
class ScraperManager;

// Tracks every kairos_id that was live at the end of a full sync run.
// Passed through syncContent → syncShows / syncMovies so the global
// orphan cleanup that runs after all sources have a complete picture.
struct SyncLiveIds {
    std::unordered_set<std::string> shows;
    std::unordered_set<std::string> episodes;
    std::unordered_set<std::string> movies;

    // Per-source subsets: which kairos_ids each source reported this run.
    // Used to prune stale source_mapping entries without touching rows that
    // another source still claims.
    std::unordered_map<std::string, std::unordered_set<std::string>> by_source_shows;
    std::unordered_map<std::string, std::unordered_set<std::string>> by_source_episodes;
    std::unordered_map<std::string, std::unordered_set<std::string>> by_source_movies;
};

class SyncManager {
public:
    SyncManager(Database& db, ConfStore& conf);

    void loadSources();
    void syncAll();
    void syncSource(const std::string& source_id);

    // Kicks off a background sync on a detached thread; no-ops if already running
    void triggerSync(const std::string& source_id = "");
    bool isSyncing() const { return sync_running_.load(); }

    // Re-syncs all Plex-linked playlists/filler-lists without a full library scan
    void triggerPlexLinkSync();
    bool isPlexLinkSyncing() const { return plex_sync_running_.load(); }

    // True while any sync/match/chapter phase is in progress.
    // Media mutations (PATCH show/movie, chapter edits) are blocked while locked.
    bool isMediaLocked() const { return media_locked_.load(); }

    std::vector<std::string> sourceIds()                              const;
    IMediaSource*             findSource(const std::string& source_id) const;

    int  getThreadCount() const;
    void setThreadCount(int n);

    void requestYield() { yield_requested_.store(true,  std::memory_order_relaxed); }
    void clearYield()   { yield_requested_.store(false, std::memory_order_relaxed); }

    void setScraperManager(ScraperManager* s) { scraper_ = s; }

    void syncItemChapters(IMediaSource& src,
                          const std::string& media_type,
                          const std::string& kairos_id,
                          const std::string& external_id,
                          const std::string& file_path);

private:
    void syncContent(const std::string& source_id, SyncLiveIds& live);
    void syncPlexLinks(const std::string& source_id);

    void syncShows(IMediaSource& src,
                   const std::string& source_id,
                   const std::string& library_id,
                   const std::string& external_lib_id,
                   const std::string& label,
                   SyncLiveIds& live);

    void syncMovies(IMediaSource& src,
                    const std::string& source_id,
                    const std::string& library_id,
                    const std::string& external_lib_id,
                    const std::string& label,
                    SyncLiveIds& live);

    // Runs after all sources finish: prunes stale source_mapping entries
    // per source, then deletes media rows that have no remaining mappings.
    void runOrphanCleanup(const SyncLiveIds& live);

    void syncChaptersFromFiles(const std::string& source_id);

    std::unique_ptr<IMediaSource> buildSource(const std::string& source_id,
                                             const std::string& source_type,
                                             const std::string& base_url) const;

    void yieldIfRequested();

    Database&                                  db_;
    ConfStore&                                 conf_;
    SQLite::Database                           sync_db_;
    std::vector<std::unique_ptr<IMediaSource>> sources_;
    std::atomic<bool>                          sync_running_{false};
    std::atomic<bool>                          plex_sync_running_{false};
    std::atomic<bool>                          media_locked_{false};
    std::atomic<int>                           override_thread_count_{0};
    std::atomic<bool>                          yield_requested_{false};
    ScraperManager*                            scraper_{nullptr};
};
