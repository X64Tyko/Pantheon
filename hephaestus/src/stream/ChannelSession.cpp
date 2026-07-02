#include "ChannelSession.h"
#include "EncoderArgs.h"
#include "MediaProbe.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <filesystem>

using clock_t_ = std::chrono::system_clock;
using steady_  = std::chrono::steady_clock;

// How long start() waits for the initial /now lookup before falling back to
// the splash. Kairos normally answers in low single-digit ms — far faster
// than ffmpeg can fork/exec/init — so unconditionally spawning the splash
// first (the original design) meant it was always killed and replaced before
// it ever produced a frame: zero visible benefit, plus a wasted spawn. Only
// engage the splash for genuine cold-start/network slowness, which is what
// Hermes's ChannelBroadcaster retry/backoff already budgets several seconds for.
static constexpr auto kFastPathBudget = std::chrono::milliseconds(250);

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        clock_t_::now().time_since_epoch()).count();
}

// Drift-to-speed correction tuning. Kept conservative/near-imperceptible:
// only ever nudge playback by up to 2%, and only attempt it when the drift
// is small enough that a 2% nudge over the item's own duration is actually
// enough to close it — otherwise fall back to seeking/skipping content.
static constexpr double kMinSpeed = 0.98;
static constexpr double kMaxSpeed = 1.02;

// Shared by appendOutputArgs' -hls_time and pushVideoEncoderArgs'
// -force_key_frames so the two can never drift apart the way they did
// before (HLS can only cut a segment at a keyframe, so a keyframe interval
// looser than -hls_time silently overrides it).
static constexpr int kLiveHlsSegmentSecs = 2;

// ── ffmpeg arg construction ───────────────────────────────────────────────────
// pushVideoEncoderArgs()/pushAudioEncoderArgs()/fmtSpeed() live in
// EncoderArgs.h/.cpp so VodSession can share them too.

// Appends the final output-format arguments. When hls_dir is non-empty, uses
// ffmpeg's tee muxer to duplicate the single encode to both the existing
// MPEG-TS stdout pipe (Hermes fan-out / DVR clients, unchanged) and a rolling
// HLS segment window on disk (web player). hls_dir must already exist —
// ffmpeg's hls muxer does not create directories.
static void appendOutputArgs(std::vector<std::string>& a, const std::string& hls_dir) {
    if (hls_dir.empty()) {
        a.insert(a.end(), {"-avoid_negative_ts", "make_zero",
                            "-muxdelay", "0", "-muxpreload", "0"});
        a.insert(a.end(), {"-f", "mpegts", "pipe:1"});
        return;
    }
    // Per-branch muxer options must live inside that branch's [brackets] —
    // global options placed before -f tee do not propagate into sub-muxers.
    // muxdelay/muxpreload are rejected as unknown options inside a tee
    // bracket (verified against ffmpeg n8.1.1) even though they're valid
    // top-level mpegts options; avoid_negative_ts alone works.
    //
    // hls_time=2 (not the more typical 4-6s): the live encode is paced with
    // -re, so ffmpeg only closes/flushes segment 0 once hls_time seconds of
    // real wall-clock content has played — a first-tune-in cold start is
    // gated on that plus process spawn/codec-init overhead before any HLS
    // output exists at all. Measured ~4-5s with hls_time=4, which was
    // routinely losing the race against the client's manifest-load patience
    // (see Router.cpp's waitForFile). hls_time=2 roughly halves that floor.
    std::string spec =
        "[f=mpegts:avoid_negative_ts=make_zero]pipe:1"
        "|[f=hls:hls_time=" + std::to_string(kLiveHlsSegmentSecs) + ":hls_list_size=6:hls_flags=delete_segments+append_list"
        ":hls_segment_filename=" + hls_dir + "/seg-%05d.ts]" + hls_dir + "/playlist.m3u8";
    a.insert(a.end(), {"-f", "tee", spec});
}

static std::vector<std::string> buildArgs(
    const std::string& ffmpeg_path,
    const KairosNowResponse& item,
    int64_t startOffsetMs,
    int audioTrackIndex,
    int subtitleTrackIndex,
    bool loudnorm,
    HwAccel hw_accel,
    const std::string& vaapi_device,
    double speed = 1.0,
    const std::string& max_resolution = "source",
    int video_bitrate_kbps = 0,
    int audio_bitrate_kbps = 192,
    const std::string& hls_dir = "")
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

    pushHwAccelDecodeArgs(a, hw_accel);

    a.push_back("-i"); a.push_back(item.file_path);

    // Stream selection: first video (optional), selected audio track (optional)
    a.insert(a.end(), {"-map", "0:v:0?",
                        "-map", "0:a:" + std::to_string(audioTrackIndex) + "?"});
    if (subtitleTrackIndex >= 0)
        a.insert(a.end(), {"-map", "0:s:" + std::to_string(subtitleTrackIndex) + "?"});

    // No data streams, no chapter metadata in output
    a.insert(a.end(), {"-dn", "-map_chapters", "-1"});

    // Video encoder
    std::vector<std::string> vfParts;
    // Scale down to the configured max resolution — never upscales.
    // scale=-2:min(ih,N) keeps aspect ratio (width auto-rounded to even).
    if (max_resolution == "1080p")     vfParts.push_back("scale=-2:min(ih\\,1080)");
    else if (max_resolution == "720p") vfParts.push_back("scale=-2:min(ih\\,720)");
    else if (max_resolution == "480p") vfParts.push_back("scale=-2:min(ih\\,480)");
    if (speed != 1.0) vfParts.push_back("setpts=PTS/" + fmtSpeed(speed));

    pushVideoEncoderArgs(a, vfParts, hw_accel, kLiveHlsSegmentSecs);
    pushVideoFilterArgs(a, vfParts);
    // Optional bitrate cap: keeps CRF quality-based encoding but adds an
    // upper bound, preventing huge spikes on complex/high-res content.
    if (video_bitrate_kbps > 0) {
        std::string maxrate = std::to_string(video_bitrate_kbps) + "k";
        std::string bufsize = std::to_string(video_bitrate_kbps * 2) + "k";
        a.insert(a.end(), {"-maxrate", maxrate, "-bufsize", bufsize});
    }

    // Audio: AAC
    pushAudioEncoderArgs(a, loudnorm, speed, audio_bitrate_kbps);

    appendOutputArgs(a, hls_dir);
    return a;
}

// Loops a still image into an MPEG-TS stream, with either a looped audio
// track or generated silence. Used for the offline slate and the connect-time
// splash — neither has a real media file to seek/map into like buildArgs().
static std::vector<std::string> buildImageArgs(
    const std::string& ffmpeg_path,
    const std::string& image_path,
    const std::string& audio_path, // empty = generate silence
    HwAccel hw_accel,
    const std::string& vaapi_device,
    const std::string& max_resolution,
    int video_bitrate_kbps,
    int audio_bitrate_kbps,
    const std::string& hls_dir = "")
{
    std::vector<std::string> a;
    a.push_back(ffmpeg_path);

    a.insert(a.end(), {"-fflags", "+genpts", "-flags", "low_delay"});
    a.push_back("-re");

    if (hw_accel == HwAccel::amd)
        a.insert(a.end(), {"-vaapi_device", vaapi_device});

    // Input 0: the image, looped forever. A still image has no frame rate of
    // its own — pick a normal one so players see a standard CFR stream.
    a.insert(a.end(), {"-loop", "1", "-framerate", "25", "-i", image_path});

    // Input 1: configured offline audio (looped) or generated silence.
    if (!audio_path.empty())
        a.insert(a.end(), {"-stream_loop", "-1", "-i", audio_path});
    else
        a.insert(a.end(), {"-f", "lavfi", "-i", "anullsrc=channel_layout=stereo:sample_rate=48000"});

    a.insert(a.end(), {"-map", "0:v:0", "-map", "1:a:0"});
    a.insert(a.end(), {"-dn", "-map_chapters", "-1"});

    std::vector<std::string> vfParts;
    if (max_resolution == "1080p")     vfParts.push_back("scale=-2:min(ih\\,1080)");
    else if (max_resolution == "720p") vfParts.push_back("scale=-2:min(ih\\,720)");
    else if (max_resolution == "480p") vfParts.push_back("scale=-2:min(ih\\,480)");

    pushVideoEncoderArgs(a, vfParts, hw_accel, kLiveHlsSegmentSecs);
    pushVideoFilterArgs(a, vfParts);
    if (video_bitrate_kbps > 0) {
        std::string maxrate = std::to_string(video_bitrate_kbps) + "k";
        std::string bufsize = std::to_string(video_bitrate_kbps * 2) + "k";
        a.insert(a.end(), {"-maxrate", maxrate, "-bufsize", bufsize});
    }

    pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, audio_bitrate_kbps);

    appendOutputArgs(a, hls_dir);
    return a;
}

// ── ChannelSession ────────────────────────────────────────────────────────────

ChannelSession::ChannelSession(std::string channel_id, KairosClient& kairos,
                                std::string ffmpeg_path, StreamOptions opts)
    : channel_id(std::move(channel_id))
    , kairos(kairos)
    , ffmpeg_path(std::move(ffmpeg_path))
    , opts(std::move(opts)) {}

ChannelSession::~ChannelSession() {
    stop();
    hls_watcher_stop.store(true);
    if (hls_watcher.joinable()) hls_watcher.join();
}

std::string ChannelSession::hlsDir() const {
    if (opts.hls_root.empty()) return "";
    return opts.hls_root + "/live/" + channel_id;
}

void ChannelSession::touchHls() {
    last_hls_touch_ms.store(nowMs());
}

// True when HLS is disabled, or has never been touched, or hasn't been
// touched within the linger window — i.e. it's safe to stop on HLS grounds.
bool ChannelSession::hlsIdle() const {
    if (opts.hls_root.empty()) return true;
    int64_t touch = last_hls_touch_ms.load();
    if (touch == 0) return true;
    return (nowMs() - touch) > static_cast<int64_t>(opts.linger_secs) * 1000;
}

// Polls because HLS itself is poll-based — there's no persistent connection
// to react to the way removeClient() reacts to an MPEG-TS client dropping.
// Without this, a channel watched only via HLS (zero MPEG-TS ClientSinks)
// would never trigger scheduleStop() at all and run forever.
void ChannelSession::hlsWatchLoop() {
    while (!hls_watcher_stop.load() && active.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!active.load()) return;
        if (client_count.load() == 0 && hlsIdle()) { stop(); return; }
    }
}

// Computes how far into `item` playback should start, given true wall-clock
// time `atMs`. Fillers loop on their own duration; non-fillers clamp to 0.
int64_t ChannelSession::computeOffset(const KairosNowResponse& item, int64_t atMs) {
    int64_t rawOffset = std::max(int64_t(0), atMs - item.wall_clock_start_ms);
    return (item.is_filler && item.duration_ms > 0)
        ? rawOffset % item.duration_ms
        : rawOffset;
}

std::optional<double> ChannelSession::computeSpeed(int64_t rawDriftMs, int64_t durationMs) {
    if (durationMs <= 0 || rawDriftMs == 0) return std::nullopt;

    // Playing at duration / (duration - drift) means the item takes exactly
    // (duration - drift) ms of wall-clock time to play `duration` ms of
    // content — i.e. the gap is fully closed by the time it ends. Positive
    // drift (running behind schedule) yields speed > 1 (speed up); negative
    // drift (running ahead) yields speed < 1 (slow down).
    double speed = static_cast<double>(durationMs) /
                   static_cast<double>(durationMs - rawDriftMs);

    // Only apply the correction if it's small enough to stay within our
    // near-imperceptible clamp unclamped — clamping a correction that needed
    // to be much larger would apply an audible speed change while still
    // failing to close the gap, which is strictly worse than just seeking.
    if (speed < kMinSpeed || speed > kMaxSpeed) return std::nullopt;

    return speed;
}

bool ChannelSession::start() {
    active    = true;
    in_splash = false;

    auto dir = hlsDir();
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::cerr << "[session:" << channel_id << "] failed to create HLS dir \""
                      << dir << "\": " << ec.message() << " — HLS output disabled for this session\n";
            opts.hls_root.clear();
        } else {
            hls_watcher = std::thread([this] { hlsWatchLoop(); });
        }
    }

    // Kick off the /now lookup on its own thread and give it a short budget
    // to answer before deciding whether to show the splash at all.
    int64_t at = nowMs();
    auto prom = std::make_shared<std::promise<std::optional<KairosNowResponse>>>();
    std::future<std::optional<KairosNowResponse>> fut = prom->get_future();
    std::thread([this, prom, at] {
        prom->set_value(kairos.getNow(channel_id, at));
    }).detach();

    if (fut.wait_for(kFastPathBudget) == std::future_status::ready) {
        // Fast path (the common case): apply the already-resolved item directly,
        // never touching the splash.
        applyResolvedItem(fut.get(), at);
    } else {
        // Kairos hasn't answered within budget (cold start / network blip) — show
        // the splash now so the client gets bytes immediately, and swap over once
        // the lookup (still in flight on its own thread) finally completes.
        in_splash = true;
        spawnOffline(KairosNowResponse{});
        std::thread([this, fut = std::move(fut), at]() mutable {
            applyResolvedItem(fut.get(), at);
        }).detach();
    }

    return active.load();
}

void ChannelSession::applyResolvedItem(std::optional<KairosNowResponse> item, int64_t at) {
    if (!item) {
        std::cerr << "[session:" << channel_id << "] /now failed at startup\n";
        if (!in_splash.load()) {
            in_splash = true;
            spawnOffline(KairosNowResponse{});
        }
        return;
    }

    // Compute how far into the item we are
    int64_t startOffset = computeOffset(*item, at);

    current_item            = *item;
    item_start               = steady_::now();
    current_item_offset_ms   = startOffset;
    in_splash                = false;

    // Prefer a gentle speed correction (whole item plays start-to-finish,
    // just slightly faster/slower) over seeking into content when the drift
    // is small enough to close that way.
    double speed = 1.0;
    if (!item->is_filler && item->duration_ms > 0) {
        int64_t rawDrift = at - item->wall_clock_start_ms;
        if (auto s = computeSpeed(rawDrift, item->duration_ms)) {
            speed = *s;
            startOffset = 0;
            current_item_offset_ms = 0;
        }
    }

    // If the offset meets or exceeds the item's known duration, ffmpeg would
    // seek past EOF and exit immediately with code=0, looping indefinitely.
    // Skip to transition directly instead.
    if (!item->is_filler && item->duration_ms > 0 && startOffset >= item->duration_ms) {
        std::cerr << "[session:" << channel_id << "] startup offset " << startOffset
                  << "ms >= duration " << item->duration_ms << "ms for \""
                  << item->file_path << "\", skipping to next item\n";
        transition(); // already off the connect thread, no need to detach again
        return;
    }

    spawnFfmpeg(*item, startOffset, speed);
}

void ChannelSession::stop() {
    if (!active.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(ffmpeg_mtx);
        if (ffmpeg) { ffmpeg->kill(); ffmpeg.reset(); }
    }
    broadcastDone();
    auto dir = hlsDir();
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
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

    if (in_splash.load()) {
        // The splash loops forever and should never exit on its own; if it
        // does, just respawn it — the background /now lookup from start() is
        // still running independently and will swap over once it's ready.
        std::thread([this] { spawnOffline(KairosNowResponse{}); }).detach();
        return;
    }

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

    // The item's *actual* real-time playback duration (elapsed) can drift
    // from its scheduled duration_ms — rounding, edit lists, VFR content, or
    // ffprobe estimation error at scheduling time. That drift is proportional
    // to item length, so it's invisible on short fillers but can be several
    // seconds off after a long movie/episode. Anchor "true now" on the actual
    // elapsed play time (including whatever offset the current item itself
    // started at, so drift keeps accumulating correctly across repeated
    // transitions instead of resetting each time) rather than trusting
    // wall_clock_end_ms verbatim.
    int64_t actualNowMs = current_item.wall_clock_start_ms + current_item_offset_ms + elapsed;

    // Look up the next item using the *scheduled* end time (not actualNowMs)
    // so we request it deterministically and never skip ahead into real
    // programming due to drift — only the resulting offset is drift-corrected.
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

    // If we've fallen far enough behind that we've already blown past an
    // entire filler's scheduled window, skip it outright instead of starting
    // partway into a "random" repeat of it (fillers are low-stakes/
    // interchangeable padding — unlike real programming, dropping one
    // entirely is the better trade for clawing back accumulated drift).
    // Never skips a non-filler item this way — only ever advances past
    // fillers whose entire window has already elapsed.
    int guard = 0;
    while (next->is_filler && next->duration_ms > 0 &&
           actualNowMs >= next->wall_clock_end_ms && ++guard < 64) {
        auto after = kairos.getNow(channel_id, next->wall_clock_end_ms);
        if (!after) break;
        next = after;
    }

    int64_t startOffset = computeOffset(*next, actualNowMs);

    current_item            = *next;
    item_start               = steady_::now();
    current_item_offset_ms   = startOffset;

    // Prefer a gentle speed correction over seeking into content when the
    // drift is small enough to close over the course of the whole item —
    // avoids ever cutting off the beginning of real programming just to
    // stay synced. Fillers are excluded: their own loop-offset/skip logic
    // already absorbs drift, and their short duration makes any speed nudge
    // both less effective and more perceptible.
    double speed = 1.0;
    if (!next->is_filler && next->duration_ms > 0) {
        int64_t rawDrift = actualNowMs - next->wall_clock_start_ms;
        if (auto s = computeSpeed(rawDrift, next->duration_ms)) {
            speed = *s;
            startOffset = 0;
            current_item_offset_ms = 0;
        }
    }

    // Same EOF-seek guard as start(): if drift pushed us past this item's
    // duration too, skip straight to the one after it instead of spawning
    // ffmpeg with an offset past EOF.
    if (!next->is_filler && next->duration_ms > 0 && startOffset >= next->duration_ms) {
        std::cerr << "[session:" << channel_id << "] transition offset " << startOffset
                  << "ms >= duration " << next->duration_ms << "ms for \""
                  << next->file_path << "\", skipping to next item\n";
        std::thread([this] { transition(); }).detach();
        return;
    }

    spawnFfmpeg(*next, startOffset, speed);
}

void ChannelSession::spawnFfmpeg(const KairosNowResponse& item, int64_t startOffsetMs, double speed) {
    if (item.item_type == "offline") {
        spawnOffline(item);
        return;
    }

    if (item.file_path.empty()) {
        std::cerr << "[session:" << channel_id << "] item has no file_path, skipping\n";
        active = false;
        broadcastDone();
        return;
    }

    std::cout << "[session:" << channel_id << "] spawning ffmpeg: \""
              << item.file_path << "\" offset=" << startOffsetMs << "ms";
    if (speed != 1.0)
        std::cout << " speed=" << speed;
    std::cout << "\n";

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
                          opts.loudnorm, opts.hw_accel, opts.vaapi_device, speed,
                          opts.max_resolution, opts.video_bitrate_kbps, opts.audio_bitrate_kbps,
                          hlsDir());
    launchFfmpeg(std::move(args), "ffmpeg");
}

// Resolves an image to loop for an "offline" item (dead-air slate or connect
// splash): the item's own offline_image_path, else this channel's logo, else
// the bundled generic default. Only bails with no stream if all three are
// unset, which shouldn't happen once default_logo_path is configured.
void ChannelSession::spawnOffline(const KairosNowResponse& item) {
    std::string image = item.offline_image_path.value_or("");
    if (image.empty()) image = opts.logo_path;
    if (image.empty()) image = opts.default_logo_path;

    if (image.empty()) {
        std::cerr << "[session:" << channel_id << "] no offline image available, skipping\n";
        active = false;
        broadcastDone();
        return;
    }

    std::cout << "[session:" << channel_id << "] spawning ffmpeg (offline slate): \"" << image << "\"\n";

    auto args = buildImageArgs(ffmpeg_path, image, item.offline_audio_path.value_or(""),
                               opts.hw_accel, opts.vaapi_device, opts.max_resolution,
                               opts.video_bitrate_kbps, opts.audio_bitrate_kbps, hlsDir());
    launchFfmpeg(std::move(args), "ffmpeg (offline slate)");
}

// Shared tail of spawnFfmpeg()/spawnOffline(): installs `args` as the
// session's ffmpeg process, replacing (and killing, via unique_ptr swap) any
// prior one — this is the same mechanism transition() uses between scheduled
// items, so it's also what a splash-to-real-content swap uses.
void ChannelSession::launchFfmpeg(std::vector<std::string> args, const char* what) {
    std::lock_guard<std::mutex> lock(ffmpeg_mtx);
    // Re-check active after acquiring the mutex: stop() may have run between
    // building args (which can involve an ffprobe call) and here, leaving
    // active=false and ffmpeg=nullptr.
    if (!active.load()) return;

    ffmpeg = std::make_unique<FfmpegProcess>(
        std::move(args),
        [this](const uint8_t* d, size_t l) { onData(d, l); },
        [this](int code) { onExit(code); },
        opts.buffer_size,
        opts.ffmpeg_debug_logs
    );

    if (!ffmpeg->start()) {
        std::cerr << "[session:" << channel_id << "] failed to spawn " << what << "\n";
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
        if (stop_token.load() == token && client_count.load() == 0 && hlsIdle()) {
            std::cerr << "[session:" << channel_id << "] linger expired, stopping\n";
            stop();
        }
    }).detach();
}
