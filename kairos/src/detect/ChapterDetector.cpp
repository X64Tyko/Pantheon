#include "ChapterDetector.h"
#include "log/DebugLog.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {

// Same private-copy convention as MediaProbe.cpp/DownloadManager.cpp — this
// codebase duplicates shellQuote per shell-out module rather than sharing it.
std::string shellQuote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else           r += c;
    }
    return r + "'";
}

constexpr int kSceneDetectTimeoutSecs = 900; // full-file video decode, no audio — generous ceiling
constexpr double kSceneThreshold      = 0.3; // ffmpeg select='gt(scene,N)' — standard scene-cut default

constexpr int64_t kTvTargetIntervalMs    = 11 * 60 * 1000; // ~10-12 min act breaks
constexpr int64_t kMovieTargetIntervalMs = 22 * 60 * 1000; // ~20-25 min
constexpr int64_t kAdBreakGuardMs        = 3  * 60 * 1000; // don't break in the first/last 3 min
constexpr int64_t kSearchWindowMs        = 90 * 1000;      // how far from target to look for a real cut

} // namespace

std::vector<int64_t> sceneChangeTimeline(const std::string& file_path) {
    const std::string cmd =
        "timeout -k 5 " + std::to_string(kSceneDetectTimeoutSecs) + " ffmpeg -i "
        + shellQuote(file_path)
        + " -vf \"scale=iw/4:ih/4,select='gt(scene," + std::to_string(kSceneThreshold)
        + ")',showinfo\" -f null - 2>&1";
    DLOG << "[detect] scene timeline cmd: " << cmd << '\n';
    const auto t0 = std::chrono::steady_clock::now();

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        DLOG << "[detect] scene timeline popen failed: " << file_path << '\n';
        return {};
    }
    std::vector<int64_t> cuts;
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe)) {
        const std::string line(buf);
        auto pos = line.find("pts_time:");
        if (pos == std::string::npos) continue;
        pos += std::string("pts_time:").size();
        try {
            const double secs = std::stod(line.substr(pos));
            cuts.push_back(static_cast<int64_t>(secs * 1000.0));
        } catch (...) {}
    }
    pclose(pipe);

    std::sort(cuts.begin(), cuts.end());
    DLOG << "[detect] scene timeline done in " << elapsedMs(t0, std::chrono::steady_clock::now())
         << "ms → " << cuts.size() << " cut(s): " << file_path << '\n';
    return cuts;
}

std::vector<int64_t> pickAdBreakPoints(const std::vector<int64_t>& cuts_ms,
                                       int64_t duration_ms,
                                       int64_t target_interval_ms,
                                       int64_t guard_start_ms,
                                       int64_t guard_end_ms,
                                       int64_t search_window_ms) {
    std::vector<int64_t> points;
    if (target_interval_ms <= 0 || duration_ms <= guard_start_ms + guard_end_ms)
        return points;

    // Targets are spaced from the start of the content, not from the guard
    // boundary — the guard only excludes candidate cuts near the edges, it
    // doesn't shift where breaks would otherwise fall.
    const int64_t usable_end = duration_ms - guard_end_ms;
    for (int64_t target = target_interval_ms; target < usable_end; target += target_interval_ms) {
        int64_t best = -1, best_dist = search_window_ms + 1;
        for (int64_t cut : cuts_ms) {
            if (cut < guard_start_ms || cut > usable_end) continue;
            const int64_t dist = std::llabs(cut - target);
            if (dist <= search_window_ms && dist < best_dist) { best = cut; best_dist = dist; }
        }
        if (best >= 0) points.push_back(best);
    }

    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

std::vector<Chapter> detectAdBreaks(const std::string& file_path,
                                     int64_t duration_ms,
                                     const std::string& media_type) {
    const int64_t target_interval_ms = (media_type == "movie") ? kMovieTargetIntervalMs : kTvTargetIntervalMs;
    const auto cuts   = sceneChangeTimeline(file_path);
    const auto points = pickAdBreakPoints(cuts, duration_ms, target_interval_ms,
                                           kAdBreakGuardMs, kAdBreakGuardMs, kSearchWindowMs);

    std::vector<Chapter> result;
    int pos = 0;
    for (int64_t ms : points) {
        Chapter c;
        c.chapter_type = "ad_break";
        c.source       = "detected";
        c.position     = pos++;
        c.start_ms     = ms;
        c.end_ms       = ms; // zero-length point marker — the scheduler will decide the cut duration later
        result.push_back(std::move(c));
    }
    return result;
}
