#pragma once
#include "MediaSource.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class Database;
class ConfStore;

class SyncManager {
public:
    SyncManager(Database& db, ConfStore& conf);

    void loadSources();
    void syncAll();
    void syncSource(const std::string& source_id);

    // Kicks off a background sync on a detached thread; no-ops if already running
    void triggerSync(const std::string& source_id = "");
    bool isSyncing() const { return sync_running_.load(); }

    std::vector<std::string> sourceIds()                              const;
    MediaSource*             findSource(const std::string& source_id) const;

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

    void syncShows(MediaSource& src,
                   const std::string& source_id,
                   const std::string& library_id,
                   const std::string& external_lib_id);

    void syncMovies(MediaSource& src,
                    const std::string& source_id,
                    const std::string& library_id,
                    const std::string& external_lib_id);

    std::unique_ptr<MediaSource> buildSource(const std::string& source_id,
                                             const std::string& source_type,
                                             const std::string& base_url) const;

    Database&                                 db_;
    ConfStore&                                conf_;
    std::vector<std::unique_ptr<MediaSource>> sources_;
    std::atomic<bool>                         sync_running_{false};
};
