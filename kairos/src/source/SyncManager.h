#pragma once
#include "IMediaSource.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Database;
class ConfStore;
class ScraperManager;
class ChapterDetectionManager;

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

    // Syncs just one library within a source: content ingestion (new/changed
    // shows/movies/episodes) plus scraper matching for anything newly
    // unmatched. Deliberately skips orphan cleanup and the source-wide
    // chapter/specials rescans:
    //  - runOrphanCleanup treats "not reported this run" as "removed," which
    //    is only sound when a run covered every library a source has (see
    //    its own doc comment) — running it here would delete content from
    //    this source's *other* libraries that this run never touched.
    //  - syncChaptersFromFiles/scanSpecialsForEligibleShows both scan across
    //    the whole source (or DB), not just one library — running them here
    //    would defeat the point of a fast, targeted single-library sync.
    // A removal made in this library won't be picked up until a full
    // syncSource/syncAll — this is a lighter-weight "pick up new/changed
    // content quickly" tool, not a substitute for a full sync.
    void syncLibrary(const std::string& source_id, const std::string& library_id);

    // Kicks off a background sync on a detached thread; no-ops if already
    // running. library_id restricts the run to syncLibrary() above instead
    // of the full syncSource() — source_id must be non-empty when it's set.
    void triggerSync(const std::string& source_id = "", const std::string& library_id = "");
    bool isSyncing() const { return sync_running_.load(); }

    // Which source's content-ingestion phase is running right now — empty
    // when idle, or during the phases that aren't per-source (orphan
    // cleanup, scraper matching, chapter sync). Drives the Activity page's
    // per-source "currently active" indicator.
    std::string currentSyncSourceId() const {
        std::lock_guard<std::mutex> lock(current_source_mtx_);
        return current_source_id_;
    }

    // Like triggerSync(), but first wipes source_mapping for source_id (or,
    // same as triggerSync(""), every source when left empty) so every item
    // is re-resolved from scratch (path/title dedup, fresh ids) instead of
    // trusting whatever it was already mapped to — the same resolution a
    // source gets the very first time it's ever synced. Use when mappings
    // are suspected stale/wrong rather than the library having genuinely
    // changed (that's what a normal sync already handles).
    void triggerHardSync(const std::string& source_id = "");

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

    // Late-bound same as setScraperManager above (avoids a circular include —
    // ChapterDetectionManager already depends on SyncManager). When set,
    // syncChaptersFromFiles opportunistically kicks off cross-episode
    // intro/credits/recap/etc. detection (ChapterDetector.h) for one
    // not-yet-detected show per sync pass — see its doc comment for why only
    // one, not every eligible show at once.
    void setChapterDetectionManager(ChapterDetectionManager* m) { chapter_detect_ = m; }

    void syncItemChapters(IMediaSource& src,
                          const std::string& media_type,
                          const std::string& kairos_id,
                          const std::string& external_id,
                          const std::string& file_path);

private:
    // library_id empty = every enabled library on this source (the normal
    // full-source path); non-empty = just that one (see syncLibrary above).
    void syncContent(const std::string& source_id, SyncLiveIds& live,
                     const std::string& library_id = "");
    void syncPlexLinks(const std::string& source_id);
    void clearSourceMapping(const std::string& source_id);

    void syncShows(IMediaSource& src,
                   const std::string& source_id,
                   const std::string& library_id,
                   const std::string& external_lib_id,
                   const std::string& library_type,
                   const std::string& label,
                   SyncLiveIds& live);

    void syncMovies(IMediaSource& src,
                    const std::string& source_id,
                    const std::string& library_id,
                    const std::string& external_lib_id,
                    const std::string& library_type,
                    const std::string& label,
                    SyncLiveIds& live);

    // Runs after all sources finish: prunes stale source_mapping entries
    // per source, then deletes media rows that have no remaining mappings.
    void runOrphanCleanup(const SyncLiveIds& live);

    // Runs the specials scan (ScraperManager::scanSpecialsForShow) for every
    // show that opted in (find_specials=1) and already has a confirmed match
    // (there's nothing to query a scraper with otherwise). No-op if no
    // ScraperManager is wired up.
    void scanSpecialsForEligibleShows();

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
    mutable std::mutex                         current_source_mtx_;
    std::string                                current_source_id_;
    std::atomic<bool>                          media_locked_{false};
    std::atomic<int>                           override_thread_count_{0};
    std::atomic<bool>                          yield_requested_{false};
    ScraperManager*                            scraper_{nullptr};
    ChapterDetectionManager*                   chapter_detect_{nullptr};
};
