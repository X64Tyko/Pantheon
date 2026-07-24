#include "VodEncodeStream.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// How many segments one head is responsible for before it self-terminates —
// same order of magnitude as Kyoo's own ~100-segment window. Large enough
// that a normal viewing session rarely crosses a boundary at all; small
// enough to bound worst-case wasted encode work from a head nobody ever
// finishes watching.
constexpr int kVodHeadWindowSegments = 100;

// A segment just short of a head's own generated frontier is still worth
// waiting on rather than spawning a redundant head for — same tolerance
// prepareSegment() always had for the single-process model.
constexpr int kVodCatchUpMarginSegments = 4;

// Live heads (spawned, not yet fully torn down) this stream will hold onto
// at once — a live head keeps its hardware encoder slot (NVENC/VAAPI) even
// while paused (see FfmpegProcess::pause()'s own doc comment), so this isn't
// just a process-count nicety, it's what keeps a pathological scrub pattern
// from starving every other session's transcode of slots.
constexpr int kVodMaxLiveHeads = 2;
}

VodEncodeStream::VodEncodeStream(std::string label, std::string segment_dir, std::string segment_prefix,
                                  ArgsBuilder argsBuilder, int buffer_size, bool ffmpeg_debug_logs,
                                  bool verbose_transcode_logs, int lookahead_secs, int hls_time_secs)
    : label_(std::move(label))
    , segment_dir_(std::move(segment_dir))
    , segment_prefix_(std::move(segment_prefix))
    , args_builder_(std::move(argsBuilder))
    , buffer_size_(buffer_size)
    , ffmpeg_debug_logs_(ffmpeg_debug_logs)
    , verbose_transcode_logs_(verbose_transcode_logs)
    , lookahead_secs_(lookahead_secs)
    , hls_time_secs_(hls_time_secs) {}

VodEncodeStream::~VodEncodeStream() { stop(); }

std::string VodEncodeStream::segmentPath(int index) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%05d.ts", index);
    return segment_dir_ + "/" + segment_prefix_ + buf;
}

VodEncodeStream::Head* VodEncodeStream::findCoveringHead(int segment_index) {
    Head* fallback = nullptr;
    for (auto& h : heads_) {
        if (segment_index < h->start_segment || segment_index >= h->window_end_segment) continue;
        if (segment_index <= h->highest_generated.load()) return h.get(); // already ready — best case
        if (!fallback) fallback = h.get();
    }
    return fallback;
}

void VodEncodeStream::evictOneIfAtCap() {
    if (static_cast<int>(heads_.size()) < kVodMaxLiveHeads) return;
    // Least-recently-requested head goes — the one nobody's actually near
    // anymore, not necessarily the oldest one spawned.
    auto victim = std::min_element(heads_.begin(), heads_.end(), [](const auto& a, const auto& b) {
        return a->last_requested_at_ms.load() < b->last_requested_at_ms.load();
    });
    if (victim == heads_.end()) return;
    std::cerr << "[vod-" << label_ << "] evicting head [" << (*victim)->start_segment << ","
              << (*victim)->window_end_segment << ") at live-head cap (" << kVodMaxLiveHeads << ")\n";
    if ((*victim)->ffmpeg) (*victim)->ffmpeg->kill(); // blocks until the slot is actually free
    heads_.erase(victim);
}

VodEncodeStream::Head* VodEncodeStream::spawnHead(int segment_index, int64_t position_ms,
                                                    const std::vector<int64_t>& segment_start_ms, int total_segments) {
    int window_end = std::min(segment_index + kVodHeadWindowSegments, total_segments);
    std::optional<double> window_duration_secs;
    if (window_end < total_segments)
        window_duration_secs = (segment_start_ms[static_cast<size_t>(window_end)] - position_ms) / 1000.0;

    auto trySpawn = [&](std::shared_ptr<std::atomic<bool>> exitedFlag) -> std::unique_ptr<FfmpegProcess> {
        auto args = args_builder_(segment_index, position_ms, window_duration_secs);
        auto proc = std::make_unique<FfmpegProcess>(
            std::move(args),
            /*on_data=*/nullptr, // output goes to disk, not stdout
            [exitedFlag](int) { exitedFlag->store(true); },
            buffer_size_, ffmpeg_debug_logs_, verbose_transcode_logs_
        );
        return proc->start() ? std::move(proc) : nullptr;
    };

    auto head = std::make_unique<Head>();
    head->start_segment      = segment_index;
    head->window_end_segment = window_end;
    head->last_requested.store(segment_index);
    head->last_requested_at_ms.store(nowMs());

    // Spawn before evicting anyone — same overlap reasoning as VodSession's
    // own restartAt(): a doomed-to-be-evicted head's output is already
    // superseded, so there's no reason to eat its kill() teardown wait
    // before this one even starts existing.
    head->ffmpeg = trySpawn(head->exited_naturally);
    if (!head->ffmpeg) {
        std::cerr << "[vod-" << label_ << "] failed to spawn head at segment " << segment_index << "\n";
        return nullptr;
    }

    // Short liveness probe — same rationale/timing as VodSession's own
    // overlap check: an NVENC/VAAPI slot-acquisition failure surfaces fast.
    // Only matters here if we're actually at the live-head cap (otherwise
    // there's no eviction decision riding on it); still worth doing
    // unconditionally so a doomed head never gets counted as a real one.
    constexpr int kProbeMs = 150, kPollMs = 25;
    int waited = 0;
    while (waited < kProbeMs && head->ffmpeg->isAlive()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        waited += kPollMs;
    }
    if (!head->ffmpeg->isAlive()) {
        std::cerr << "[vod-" << label_ << "] head at segment " << segment_index
                  << " died immediately (likely no free hardware encoder session) — evicting oldest live head and retrying\n";
        head->ffmpeg.reset();
        evictOneIfAtCap();
        // If we weren't at the cap, evictOneIfAtCap() was a no-op and this
        // retry is against genuinely exhausted hardware — same terminal
        // failure shape restartAt() already has for that case.
        head->ffmpeg = trySpawn(head->exited_naturally);
        if (!head->ffmpeg) {
            std::cerr << "[vod-" << label_ << "] retry also failed to spawn head at segment " << segment_index << "\n";
            return nullptr;
        }
    } else {
        evictOneIfAtCap();
    }

    heads_.push_back(std::move(head));
    return heads_.back().get();
}

VodEncodeStream::SegmentPrep VodEncodeStream::prepareSegment(int segment_index, const std::vector<int64_t>& segment_start_ms, int total_segments) {
    if (segment_index < 0 || segment_index >= total_segments) return SegmentPrep::Failed;

    std::lock_guard<std::mutex> lock(mtx_);

    Head* head = findCoveringHead(segment_index);
    if (head) {
        head->last_requested.store(segment_index);
        head->last_requested_at_ms.store(nowMs());

        int generated = head->highest_generated.load();
        if (segment_index <= generated) {
            if (head->ffmpeg && head->ffmpeg->isPaused()) head->ffmpeg->resume();
            return SegmentPrep::Ready;
        }
        // highest_generated only advances on tick()'s ~2s scan, so a segment
        // that just finished writing can briefly look "not generated" here
        // even though it already exists on disk — same staleness guard the
        // single-process model had.
        if (std::filesystem::exists(segmentPath(segment_index))) {
            if (segment_index > generated) head->highest_generated.store(segment_index);
            if (head->ffmpeg && head->ffmpeg->isPaused()) head->ffmpeg->resume();
            return SegmentPrep::Ready;
        }
        if (!head->exited_naturally->load() && segment_index <= generated + kVodCatchUpMarginSegments) {
            if (head->ffmpeg && head->ffmpeg->isPaused()) head->ffmpeg->resume();
            return SegmentPrep::WaitShort;
        }
        // This head's own run can't produce it soon (exhausted, or too far
        // behind within its own declared window) — fall through and spawn a
        // fresh head starting here. Doesn't touch this head at all; whatever
        // it already produced stays servable from disk exactly as before.
    }

    if (!spawnHead(segment_index, segment_start_ms[static_cast<size_t>(segment_index)], segment_start_ms, total_segments))
        return SegmentPrep::Failed;
    return SegmentPrep::WaitColdStart;
}

void VodEncodeStream::tick(int total_segments) {
    std::lock_guard<std::mutex> lock(mtx_);

    for (auto& h : heads_) {
        int idx = h->highest_generated.load() + 1;
        while (idx < h->window_end_segment && idx < total_segments && std::filesystem::exists(segmentPath(idx))) {
            h->highest_generated.store(idx);
            ++idx;
        }
    }

    for (auto& h : heads_) {
        if (!h->ffmpeg) continue;
        int generated = h->highest_generated.load();
        if (generated >= h->window_end_segment - 1) continue; // this head's whole window is done — nothing left to pace

        int floor = std::max(h->last_requested.load(), h->start_segment);
        int window_segments = std::max(1, lookahead_secs_ / hls_time_secs_);
        int resume_threshold_segments = std::max(1, window_segments / 2); // hysteresis: pause at full window, resume at half
        bool should_pause  = (generated - floor) >= window_segments;
        bool should_resume = (generated - floor) < resume_threshold_segments;

        if (should_pause && !h->ffmpeg->isPaused()) h->ffmpeg->pause();
        else if (should_resume && h->ffmpeg->isPaused()) h->ffmpeg->resume();
    }

    // Reap heads that finished their whole window (or crashed) and haven't
    // been asked for anything in a while — their segments stay on disk
    // (still servable, still referenced by the static playlist) regardless
    // of whether the head object itself sticks around.
    constexpr int64_t kHeadIdleReapMs = 30'000;
    int64_t now = nowMs();
    heads_.erase(std::remove_if(heads_.begin(), heads_.end(), [&](const std::unique_ptr<Head>& h) {
        bool finished = h->highest_generated.load() >= h->window_end_segment - 1 || h->exited_naturally->load();
        if (!finished) return false;
        if (now - h->last_requested_at_ms.load() < kHeadIdleReapMs) return false;
        if (h->ffmpeg) h->ffmpeg->kill();
        return true;
    }), heads_.end());
}

bool VodEncodeStream::anyHeadPaused() const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& h : heads_) if (h->ffmpeg && h->ffmpeg->isPaused()) return true;
    return false;
}

int VodEncodeStream::liveHeadCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return static_cast<int>(heads_.size());
}

int VodEncodeStream::highestGeneratedSegment() const {
    std::lock_guard<std::mutex> lock(mtx_);
    int best = -1;
    for (auto& h : heads_) best = std::max(best, h->highest_generated.load());
    return best;
}

void VodEncodeStream::stop() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& h : heads_) {
        if (h->ffmpeg) h->ffmpeg->kill();
    }
    heads_.clear();
}
