#include "ChannelSession.h"
#include "MediaProbe.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <sstream>
#include <iomanip>

using clock_t_ = std::chrono::system_clock;
using steady_  = std::chrono::steady_clock;

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        clock_t_::now().time_since_epoch()).count();
}

// ── ffmpeg arg construction ───────────────────────────────────────────────────

static std::vector<std::string> buildArgs(
    const std::string& ffmpeg_path,
    const KairosNowResponse& item,
    int64_t startOffsetMs,
    int audioTrackIndex,
    int subtitleTrackIndex,
    bool loudnorm,
    HwAccel hw_accel,
    const std::string& vaapi_device)
{
    std::vector<std::string> a;
    a.push_back(ffmpeg_path);

    // Reduce startup latency / buffer
    a.insert(a.end(), {"-fflags", "+genpts+discardcorrupt", "-flags", "low_delay"});

    // Pace input reads to the source's native frame rate. Without this ffmpeg
    // transcodes as fast as the CPU/GPU allows (often many times real-time),
    // exits almost immediately, and the session races through the entire
    // schedule instead of simulating a live broadcast.
    a.push_back("-re");

    // AMD VAAPI: expose the render node before -i so the encoder can find it
    if (hw_accel == HwAccel::amd)
        a.insert(a.end(), {"-vaapi_device", vaapi_device});

    // Seek before input for fast seek
    if (startOffsetMs > 0) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3) << (startOffsetMs / 1000.0);
        a.push_back("-ss"); a.push_back(ss.str());
    }

    a.push_back("-i"); a.push_back(item.file_path);

    // Stream selection: first video (optional), selected audio track (optional)
    a.insert(a.end(), {"-map", "0:v:0?",
                        "-map", "0:a:" + std::to_string(audioTrackIndex) + "?"});
    if (subtitleTrackIndex >= 0)
        a.insert(a.end(), {"-map", "0:s:" + std::to_string(subtitleTrackIndex) + "?"});

    // No data streams, no chapter metadata in output
    a.insert(a.end(), {"-dn", "-map_chapters", "-1"});

    // Video encoder
    switch (hw_accel) {
        case HwAccel::nvidia:
            // CPU decode → NVENC encode; works with any input codec
            a.insert(a.end(), {"-c:v", "h264_nvenc", "-preset", "p4",
                                "-rc:v", "vbr", "-cq", "23", "-pix_fmt", "yuv420p"});
            break;
        case HwAccel::amd:
            // CPU decode → upload to VAAPI → h264_vaapi encode
            a.insert(a.end(), {"-vf", "format=nv12,hwupload",
                                "-c:v", "h264_vaapi"});
            break;
        default:
            a.insert(a.end(), {"-c:v", "libx264", "-preset", "veryfast",
                                "-crf", "23", "-pix_fmt", "yuv420p"});
    }

    // Audio: AAC
    if (loudnorm) {
        a.insert(a.end(), {"-c:a", "aac", "-b:a", "192k",
                            "-af", "loudnorm=I=-16:TP=-1.5:LRA=11"});
    } else {
        a.insert(a.end(), {"-c:a", "aac", "-b:a", "192k"});
    }

    // Clean MPEG-TS timestamps
    a.insert(a.end(), {"-avoid_negative_ts", "make_zero",
                        "-muxdelay", "0", "-muxpreload", "0"});

    a.insert(a.end(), {"-f", "mpegts", "pipe:1"});
    return a;
}

// ── ChannelSession ────────────────────────────────────────────────────────────

ChannelSession::ChannelSession(std::string channel_id, KairosClient& kairos,
                                std::string ffmpeg_path, StreamOptions opts)
    : channel_id(std::move(channel_id))
    , kairos(kairos)
    , ffmpeg_path(std::move(ffmpeg_path))
    , opts(std::move(opts)) {}

ChannelSession::~ChannelSession() { stop(); }

bool ChannelSession::start() {
    int64_t at = nowMs();
    auto item = kairos.getNow(channel_id, at);
    if (!item) {
        std::cerr << "[session:" << channel_id << "] /now failed at startup\n";
        return false;
    }

    // Compute how far into the item we are
    int64_t rawOffset = std::max(int64_t(0), at - item->wall_clock_start_ms);
    int64_t startOffset = (item->is_filler && item->duration_ms > 0)
        ? rawOffset % item->duration_ms
        : rawOffset;

    current_item = *item;
    item_start   = steady_::now();
    active       = true;

    // If the offset meets or exceeds the item's known duration, ffmpeg would
    // seek past EOF and exit immediately with code=0, looping indefinitely.
    // Skip to transition directly instead.
    if (!item->is_filler && item->duration_ms > 0 && startOffset >= item->duration_ms) {
        std::cerr << "[session:" << channel_id << "] startup offset " << startOffset
                  << "ms >= duration " << item->duration_ms << "ms for \""
                  << item->file_path << "\", skipping to next item\n";
        std::thread([this] { transition(); }).detach();
        return true;
    }

    spawnFfmpeg(*item, startOffset);
    return active.load();
}

void ChannelSession::stop() {
    if (!active.exchange(false)) return;
    std::lock_guard<std::mutex> lock(ffmpeg_mtx);
    if (ffmpeg) { ffmpeg->kill(); ffmpeg.reset(); }
    broadcastDone();
}

void ChannelSession::addClient(std::shared_ptr<ClientSink> sink) {
    std::lock_guard<std::mutex> lock(clients_mtx);
    clients.push_back(sink);
    ++client_count;
}

void ChannelSession::removeClient(std::shared_ptr<ClientSink> sink) {
    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        clients.erase(std::remove(clients.begin(), clients.end(), sink), clients.end());
        --client_count;
    }
    if (client_count.load() == 0) scheduleStop();
}

void ChannelSession::onData(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(clients_mtx);
    std::vector<uint8_t> chunk(data, data + len);
    for (auto& sink : clients) {
        std::lock_guard<std::mutex> sl(sink->mtx);
        if (sink->queue.size() >= ClientSink::MAX_QUEUE) {
            // Client can't keep up — drop oldest chunk rather than blocking
            sink->queue.pop_front();
        }
        sink->queue.push_back(chunk);
        sink->cv.notify_one();
    }
}

void ChannelSession::onExit(int code) {
    if (!active.load()) return; // we were stopped intentionally

    std::cerr << "[session:" << channel_id << "] ffmpeg exited (code=" << code << ")\n";

    // Natural exit (code 0): item finished cleanly → transition.
    // Unexpected exit (code != 0): also transition to avoid black-screen.
    std::thread([this] { transition(); }).detach();
}

void ChannelSession::transition() {
    if (!active.load()) return;

    int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        steady_::now() - item_start).count();

    kairos.markPlayed(channel_id, current_item.item_type, current_item.item_id,
                      current_item.block_id, elapsed);

    // Request next item starting at the scheduled end of the completed one.
    // This is the event-driven improvement over Tunarr's wallClockEndMs timer:
    // we transition on ffmpeg exit, not on a wall-clock deadline.
    auto next = kairos.getNow(channel_id, current_item.wall_clock_end_ms);
    if (!next) {
        // Fallback: try current wall clock
        next = kairos.getNow(channel_id, nowMs());
    }
    if (!next) {
        std::cerr << "[session:" << channel_id << "] cannot get next item, stopping\n";
        active = false;
        broadcastDone();
        return;
    }

    current_item = *next;
    item_start   = steady_::now();
    spawnFfmpeg(*next, 0); // next item always starts from the beginning
}

void ChannelSession::spawnFfmpeg(const KairosNowResponse& item, int64_t startOffsetMs) {
    if (item.file_path.empty()) {
        std::cerr << "[session:" << channel_id << "] item has no file_path, skipping\n";
        active = false;
        broadcastDone();
        return;
    }

    std::cout << "[session:" << channel_id << "] spawning ffmpeg: \""
              << item.file_path << "\" offset=" << startOffsetMs << "ms\n";

    // Audio + subtitle track selection via ffprobe
    int audioTrack    = 0;
    int subtitleTrack = -1;
    if (!opts.audio_lang.empty() || !opts.subtitle_lang.empty()) {
        auto info = probeMedia(opts.ffprobe_path, item.file_path);
        if (info) {
            if (!opts.audio_lang.empty())
                audioTrack    = pickAudioTrack(*info, opts.audio_lang);
            if (!opts.subtitle_lang.empty())
                subtitleTrack = pickSubtitleTrack(*info, opts.subtitle_lang);
        }
    }

    auto args = buildArgs(ffmpeg_path, item, startOffsetMs, audioTrack, subtitleTrack,
                          opts.loudnorm, opts.hw_accel, opts.vaapi_device);

    std::lock_guard<std::mutex> lock(ffmpeg_mtx);
    // Re-check active after acquiring the mutex: stop() may have run between
    // the probe above and here, leaving active=false and ffmpeg=nullptr.
    if (!active.load()) return;

    ffmpeg = std::make_unique<FfmpegProcess>(
        std::move(args),
        [this](const uint8_t* d, size_t l) { onData(d, l); },
        [this](int code) { onExit(code); },
        opts.buffer_size,
        opts.ffmpeg_debug_logs
    );

    if (!ffmpeg->start()) {
        std::cerr << "[session:" << channel_id << "] failed to spawn ffmpeg\n";
        active = false;
        broadcastDone();
    }
}

void ChannelSession::broadcastDone() {
    std::lock_guard<std::mutex> lock(clients_mtx);
    for (auto& sink : clients) {
        std::lock_guard<std::mutex> sl(sink->mtx);
        sink->done = true;
        sink->cv.notify_all();
    }
}

void ChannelSession::scheduleStop() {
    int token = ++stop_token;
    int linger = opts.linger_secs;
    std::thread([this, token, linger] {
        std::this_thread::sleep_for(std::chrono::seconds(linger));
        if (stop_token.load() == token && client_count.load() == 0) {
            std::cerr << "[session:" << channel_id << "] linger expired, stopping\n";
            stop();
        }
    }).detach();
}
