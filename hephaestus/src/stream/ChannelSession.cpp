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

// Drift-to-speed correction tuning. Kept conservative/near-imperceptible:
// only ever nudge playback by up to 2%, and only attempt it when the drift
// is small enough that a 2% nudge over the item's own duration is actually
// enough to close it — otherwise fall back to seeking/skipping content.
static constexpr double kMinSpeed = 0.98;
static constexpr double kMaxSpeed = 1.02;

static std::string fmtSpeed(double speed) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << speed;
    return ss.str();
}

// ── ffmpeg arg construction ───────────────────────────────────────────────────

// Video encoder selection, shared by buildArgs() and buildImageArgs(). Appends
// codec args to `a` and (for AMD, which needs a CPU->VAAPI upload after any
// scale filter) an extra entry to `vfParts`.
static void pushVideoEncoderArgs(std::vector<std::string>& a, std::vector<std::string>& vfParts,
                                  HwAccel hw_accel) {
    switch (hw_accel) {
        case HwAccel::nvidia:
            a.insert(a.end(), {"-c:v", "h264_nvenc", "-preset", "p4",
                                "-rc:v", "vbr", "-cq", "23", "-pix_fmt", "yuv420p"});
            break;
        case HwAccel::amd:
            vfParts.push_back("format=nv12,hwupload");
            a.insert(a.end(), {"-c:v", "h264_vaapi"});
            break;
        default:
            a.insert(a.end(), {"-c:v", "libx264", "-preset", "veryfast",
                                "-crf", "23", "-pix_fmt", "yuv420p"});
    }
}

// Audio encoder selection, shared by buildArgs() and buildImageArgs().
static void pushAudioEncoderArgs(std::vector<std::string>& a, bool loudnorm, double speed,
                                  int audio_bitrate_kbps) {
    std::vector<std::string> afParts;
    if (loudnorm) afParts.push_back("loudnorm=I=-16:TP=-1.5:LRA=11");
    if (speed != 1.0) afParts.push_back("atempo=" + fmtSpeed(speed));

    a.insert(a.end(), {"-c:a", "aac", "-b:a", std::to_string(audio_bitrate_kbps) + "k"});
    if (!afParts.empty()) {
        std::string af;
        for (size_t i = 0; i < afParts.size(); ++i) { if (i) af += ","; af += afParts[i]; }
        a.insert(a.end(), {"-af", af});
    }
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
    int audio_bitrate_kbps = 192)
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
    std::vector<std::string> vfParts;
    // Scale down to the configured max resolution — never upscales.
    // scale=-2:min(ih,N) keeps aspect ratio (width auto-rounded to even).
    if (max_resolution == "1080p")     vfParts.push_back("scale=-2:min(ih\\,1080)");
    else if (max_resolution == "720p") vfParts.push_back("scale=-2:min(ih\\,720)");
    else if (max_resolution == "480p") vfParts.push_back("scale=-2:min(ih\\,480)");
    if (speed != 1.0) vfParts.push_back("setpts=PTS/" + fmtSpeed(speed));

    pushVideoEncoderArgs(a, vfParts, hw_accel);
    if (!vfParts.empty()) {
        std::string vf;
        for (size_t i = 0; i < vfParts.size(); ++i) { if (i) vf += ","; vf += vfParts[i]; }
        a.insert(a.end(), {"-vf", vf});
    }
    // Optional bitrate cap: keeps CRF quality-based encoding but adds an
    // upper bound, preventing huge spikes on complex/high-res content.
    if (video_bitrate_kbps > 0) {
        std::string maxrate = std::to_string(video_bitrate_kbps) + "k";
        std::string bufsize = std::to_string(video_bitrate_kbps * 2) + "k";
        a.insert(a.end(), {"-maxrate", maxrate, "-bufsize", bufsize});
    }

    // Audio: AAC
    pushAudioEncoderArgs(a, loudnorm, speed, audio_bitrate_kbps);

    // Clean MPEG-TS timestamps
    a.insert(a.end(), {"-avoid_negative_ts", "make_zero",
                        "-muxdelay", "0", "-muxpreload", "0"});

    a.insert(a.end(), {"-f", "mpegts", "pipe:1"});
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
    int audio_bitrate_kbps)
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

    pushVideoEncoderArgs(a, vfParts, hw_accel);
    if (!vfParts.empty()) {
        std::string vf;
        for (size_t i = 0; i < vfParts.size(); ++i) { if (i) vf += ","; vf += vfParts[i]; }
        a.insert(a.end(), {"-vf", vf});
    }
    if (video_bitrate_kbps > 0) {
        std::string maxrate = std::to_string(video_bitrate_kbps) + "k";
        std::string bufsize = std::to_string(video_bitrate_kbps * 2) + "k";
        a.insert(a.end(), {"-maxrate", maxrate, "-bufsize", bufsize});
    }

    pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, audio_bitrate_kbps);

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
    in_splash = true;

    // Show this channel's logo (or the bundled default) immediately so the
    // client gets bytes right away, instead of blocking on the Kairos /now
    // round-trip + ffprobe + real ffmpeg startup below. resolveRealContent()
    // swaps over to the real item once it's ready.
    spawnOffline(KairosNowResponse{});

    std::thread([this] { resolveRealContent(); }).detach();
    return active.load();
}

// Looks up the real current item and swaps the session's ffmpeg process over
// to it, replacing whatever the connect-time splash spawned. Runs on its own
// thread so start() can return immediately (see start()'s comment).
void ChannelSession::resolveRealContent() {
    int64_t at = nowMs();
    auto item = kairos.getNow(channel_id, at);
    if (!item) {
        std::cerr << "[session:" << channel_id << "] /now failed at startup, staying on splash\n";
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

    if (in_splash.load()) {
        // The splash loops forever and should never exit on its own; if it
        // does, just respawn it — resolveRealContent() is still running
        // independently and will swap over once the real item is ready.
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
                          opts.max_resolution, opts.video_bitrate_kbps, opts.audio_bitrate_kbps);
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
                               opts.video_bitrate_kbps, opts.audio_bitrate_kbps);
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
        if (stop_token.load() == token && client_count.load() == 0) {
            std::cerr << "[session:" << channel_id << "] linger expired, stopping\n";
            stop();
        }
    }).detach();
}
