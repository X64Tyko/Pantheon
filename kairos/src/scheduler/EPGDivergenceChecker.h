#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class EPGMaterializer;

struct DivergenceCheckJob {
    std::string id;
    std::string channel_id;
    std::string status;             // "queued" | "running" | "done" | "error"
    std::string stage;              // "anchor" | "deep" — last stage reached
    bool        anchor_checked   = false; // false when checkAnchorDivergence() had nothing to compare
    bool        anchor_diverged  = false; // only meaningful when anchor_checked
    bool        deep_ran         = false;
    int         divergence_count = 0;     // only meaningful when deep_ran
    std::string started_at;               // ISO-8601 UTC
    std::string finished_at;              // ISO-8601 UTC, empty while running
    std::string error;
};

// Runs EPGMaterializer's two-tier divergence check (cheap anchor comparison,
// falling through to a full projection + item diff only when the anchor doesn't
// already confirm the cached schedule is still consistent) on a background
// thread, modeled on DownloadManager's job pattern.
class EPGDivergenceChecker {
public:
    explicit EPGDivergenceChecker(EPGMaterializer& materializer);

    // Starts a check for channel_id and returns its job id immediately. If a
    // check for this channel is already running, returns that job's id instead
    // of starting a redundant duplicate.
    std::string startCheck(const std::string& channel_id);

    // Snapshot of all jobs, newest first.
    std::vector<DivergenceCheckJob> getJobs() const;

private:
    void runCheck(std::string id);
    std::deque<DivergenceCheckJob>::iterator findJob(const std::string& id);

    EPGMaterializer& materializer_;
    mutable std::mutex             mu_;
    std::deque<DivergenceCheckJob> jobs_; // oldest at front, capped at 50
};