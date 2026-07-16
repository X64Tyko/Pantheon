#include "EPGDivergenceChecker.h"
#include "EPGMaterializer.h"
#include "Rng.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

namespace {

std::string generateId() {
    thread_local Xoshiro256 rng(static_cast<uint64_t>(std::random_device{}()));
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << rng();
    return ss.str();
}

std::string nowIso() {
    auto now      = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm     tm{};
    gmtime_r(&t, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// Deep check horizon: long enough to cover what a channel's near-term cached
// EPG actually spans, short enough to keep the background pass quick.
constexpr int kDeepCheckHorizonHours = 7 * 24;

} // namespace

EPGDivergenceChecker::EPGDivergenceChecker(EPGMaterializer& materializer)
    : materializer_(materializer) {}

std::string EPGDivergenceChecker::startCheck(const std::string& channel_id) {
    {
        std::lock_guard lk(mu_);
        for (const auto& j : jobs_)
            if (j.channel_id == channel_id && (j.status == "queued" || j.status == "running"))
                return j.id;

        // Prune the oldest finished job if at capacity.
        if (jobs_.size() >= 50) {
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                if (it->status == "done" || it->status == "error") {
                    jobs_.erase(it);
                    break;
                }
            }
        }
    }

    std::string id = generateId();
    {
        std::lock_guard lk(mu_);
        DivergenceCheckJob job;
        job.id         = id;
        job.channel_id = channel_id;
        job.status     = "queued";
        job.started_at = nowIso();
        jobs_.push_back(std::move(job));
    }
    std::thread([this, id]{ runCheck(id); }).detach();
    return id;
}

std::vector<DivergenceCheckJob> EPGDivergenceChecker::getJobs() const {
    std::lock_guard lk(mu_);
    return std::vector<DivergenceCheckJob>(jobs_.rbegin(), jobs_.rend());
}

std::deque<DivergenceCheckJob>::iterator EPGDivergenceChecker::findJob(const std::string& id) {
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
        if (it->id == id) return it;
    return jobs_.end();
}

void EPGDivergenceChecker::runCheck(std::string id) {
    std::string channel_id;
    {
        std::lock_guard lk(mu_);
        auto it = findJob(id);
        if (it == jobs_.end()) return;
        it->status = "running";
        it->stage  = "anchor";
        channel_id = it->channel_id;
    }

    try {
        auto anchor_result = materializer_.checkAnchorDivergence(channel_id, std::time(nullptr));

        bool run_deep = true;
        {
            std::lock_guard lk(mu_);
            auto it = findJob(id);
            if (it == jobs_.end()) return;
            it->anchor_checked = anchor_result.has_value();
            if (anchor_result) {
                it->anchor_diverged = !*anchor_result;
                // Anchor already confirms the cached schedule is still
                // consistent — skip the expensive full comparison.
                run_deep = *anchor_result == false;
            }
        }

        if (run_deep) {
            {
                std::lock_guard lk(mu_);
                auto it = findJob(id);
                if (it == jobs_.end()) return;
                it->stage = "deep";
            }
            auto result = materializer_.generate(channel_id, std::time(nullptr), kDeepCheckHorizonHours);

            std::lock_guard lk(mu_);
            auto it = findJob(id);
            if (it == jobs_.end()) return;
            it->deep_ran         = true;
            it->divergence_count = static_cast<int>(result.divergences.size());
        }

        std::lock_guard lk(mu_);
        auto it = findJob(id);
        if (it != jobs_.end()) {
            it->status      = "done";
            it->finished_at = nowIso();
        }
    } catch (const std::exception& e) {
        std::lock_guard lk(mu_);
        auto it = findJob(id);
        if (it != jobs_.end()) {
            it->status      = "error";
            it->error       = e.what();
            it->finished_at = nowIso();
        }
        std::cerr << "[epg] divergence check failed channel=" << channel_id
                  << ": " << e.what() << '\n';
    }
}