#pragma once
#include <atomic>
#include <string>

class Database;
class ConfStore;
class SyncManager;

// Orchestrates chapter-structure detection (ChapterDetector.h's algorithms).
// Single-flight like ScraperManager — detection is CPU/IO-heavy and a rare,
// deliberate admin action, not something that needs concurrent multi-item runs.
class ChapterDetectionManager {
public:
    ChapterDetectionManager(Database& db, ConfStore& conf, SyncManager& sync);

    // False (no-op) if a detection pass is already running.
    bool triggerShowDetect(const std::string& show_id);
    bool triggerMovieDetect(const std::string& movie_id);

    bool isDetecting() const { return detecting_.load(); }

private:
    void runShowDetect(const std::string& show_id);
    void runMovieDetect(const std::string& movie_id);

    Database&    db_;
    ConfStore&   conf_;
    SyncManager& sync_;
    std::atomic<bool> detecting_{false};
};
