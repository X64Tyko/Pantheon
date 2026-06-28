#pragma once
#include "IMediaSource.h"
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

    Database&                                  db_;
    ConfStore&                                 conf_;
    std::vector<std::unique_ptr<IMediaSource>> sources_;
    std::atomic<bool>                          sync_running_{false};
    std::atomic<bool>                          plex_sync_running_{false};
    std::atomic<int>                           override_thread_count_{0};
    ScraperManager*                            scraper_{nullptr};
};
