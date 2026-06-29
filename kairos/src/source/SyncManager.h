#pragma once
#include "IMediaSource.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class Database;
class ConfStore;
class ScraperManager;

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

    std::vector<std::string> sourceIds()                              const;
    IMediaSource*             findSource(const std::string& source_id) const;

    int  getThreadCount() const;
    void setThreadCount(int n);

    // Coordinator interface: request a brief pause between sync write transactions
    // so the HTTP thread pool has a guaranteed window to acquire the DB write lock.
    // requestYield() sets the flag; clearYield() releases it.  Sync checks the flag
    // at every transaction boundary and sleeps until it is cleared.
    void requestYield() { yield_requested_.store(true,  std::memory_order_relaxed); }
    void clearYield()   { yield_requested_.store(false, std::memory_order_relaxed); }

    // Optional scraper hook — fires triggerMatch() after each source sync completes.
    void setScraperManager(ScraperManager* s) { scraper_ = s; }

    // Sync chapters for a single item from all available sources (file + source API).
    // Called by ChapterService for per-item sync endpoint.
    void syncItemChapters(IMediaSource& src,
                          const std::string& media_type,
                          const std::string& kairos_id,
                          const std::string& external_id,
                          const std::string& file_path);

private:
    // ID resolution: looks up existing kairos_id in source_mapping, or generates one
    std::string resolveId(const std::string& item_type,
                          const std::string& source_id,
                          const std::string& external_id) const;

    void upsertMapping(const std::string& item_type,
                       const std::string& kairos_id,
                       const std::string& source_id,
                       const std::string& library_id,
                       const std::string& external_id);

    // Cross-source dedup: find an existing kairos_id by file_path (episode/movie)
    // or by case-insensitive title (show/movie). Used so local-source scans reuse
    // items already indexed by Plex/Jellyfin rather than creating duplicates.
    std::string resolveByFilePath(const std::string& item_type,
                                  const std::string& file_path) const;
    std::string resolveByTitle(const std::string& item_type,
                               const std::string& title) const;

    // Content-only phase: shows + movies + plex links. No match or chapters.
    void syncContent(const std::string& source_id);

    void syncPlexLinks(const std::string& source_id);

    void syncShows(IMediaSource& src,
                   const std::string& source_id,
                   const std::string& library_id,
                   const std::string& external_lib_id);

    void syncMovies(IMediaSource& src,
                    const std::string& source_id,
                    const std::string& library_id,
                    const std::string& external_lib_id);

    // Chapter sync — called at end of syncSource if chapter_sync_enabled.
    // File chapters (ffprobe) only; per-item source API sync is on-demand via API.
    void syncChaptersFromFiles(const std::string& source_id);

    std::unique_ptr<IMediaSource> buildSource(const std::string& source_id,
                                             const std::string& source_type,
                                             const std::string& base_url) const;

    // Sleeps at transaction boundaries while yield_requested_ is set.
    // Called by sync before acquiring each write transaction so the coordinator
    // can create a brief DB-write window for the HTTP thread pool.
    void yieldIfRequested();

    Database&                                  db_;
    ConfStore&                                 conf_;
    SQLite::Database                           sync_db_; // dedicated connection for sync writes
    std::vector<std::unique_ptr<IMediaSource>> sources_;
    std::atomic<bool>                          sync_running_{false};
    std::atomic<bool>                          plex_sync_running_{false};
    std::atomic<int>                           override_thread_count_{0};
    std::atomic<bool>                          yield_requested_{false};
    ScraperManager*                            scraper_{nullptr};
};
