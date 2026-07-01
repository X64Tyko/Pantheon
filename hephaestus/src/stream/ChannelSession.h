#pragma once
#include "../kairos/KairosClient.h"
#include "../kairos/KairosTypes.h"
#include "FfmpegProcess.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>

// One ClientSink per connected HTTP client. Thread-safe queue the HTTP handler
// thread reads from while the session's reader thread writes to it.
struct ClientSink {
    std::mutex              mtx;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> queue;
    std::atomic<bool>       done{false};
    static constexpr size_t MAX_QUEUE = 64; // ~8 MB at 128 KB chunks; slow clients are dropped
};

enum class HwAccel { none, nvidia, amd };

struct StreamOptions {
    std::string ffprobe_path      = "ffprobe";
    std::string audio_lang        = "eng";
    std::string subtitle_lang     = "";   // empty = no subtitle mapping
    bool        loudnorm          = false;
    int         linger_secs       = 60;
	int		buffer_size		  = 1048576; // 1024 KB
    HwAccel     hw_accel          = HwAccel::none;
    std::string vaapi_device      = "/dev/dri/renderD128";
    bool        ffmpeg_debug_logs = false; // pipe ffmpeg stderr into the log stream
    // Per-channel transcode quality
    std::string max_resolution    = "source"; // "source"|"1080p"|"720p"|"480p"
    int         video_bitrate_kbps = 0;       // 0 = CRF/CQ auto; >0 adds -maxrate cap
    int         audio_bitrate_kbps = 192;     // kbps for -b:a
    // Offline/splash image fallback
    std::string logo_path;          // per-channel logo, empty = none configured
    std::string default_logo_path;  // bundled generic fallback, always set
};

class ChannelSession {
    std::string   channel_id;  // Kairos channel UUID
    KairosClient& kairos;
    std::string   ffmpeg_path;
    StreamOptions opts;

    std::mutex   clients_mtx;
    std::vector<std::shared_ptr<ClientSink>> clients;
    std::atomic<int>  client_count{0};
    std::atomic<int>  stop_token{0};       // incremented on each scheduleStop call

    std::mutex   ffmpeg_mtx;
    std::unique_ptr<FfmpegProcess> ffmpeg;

    KairosNowResponse current_item;
    std::chrono::steady_clock::time_point item_start;
    int64_t current_item_offset_ms = 0; // offset into current_item's own file we started at

    std::atomic<bool> active{false};
    std::atomic<bool> in_splash{false}; // true while showing the connect-time logo splash

    void onData(const uint8_t* data, size_t len);
    void onExit(int code);
    void transition();
    // Applies a resolved /now lookup (or its absence, on failure): computes
    // start offset/speed and spawns ffmpeg for it, or falls back to the
    // splash. Shared by start()'s fast (answered before kFastPathBudget) and
    // slow (answered later, on its own thread) paths.
    void applyResolvedItem(std::optional<KairosNowResponse> item, int64_t at);
    void spawnFfmpeg(const KairosNowResponse& item, int64_t startOffsetMs, double speed = 1.0);
    void spawnOffline(const KairosNowResponse& item);
    void launchFfmpeg(std::vector<std::string> args, const char* what);
    void broadcastDone();
    void scheduleStop();

    // Computes how far into `item` playback should start, given the true
    // wall-clock time `atMs`. Loops fillers on their own duration; clamps to 0
    // for non-fillers that haven't started yet.
    static int64_t computeOffset(const KairosNowResponse& item, int64_t atMs);

    // Given raw (unclamped, signed) drift = actualNowMs - item.wall_clock_start_ms
    // and the item's known duration, decides whether the drift can be closed
    // gently over the course of playing the *entire* item at a slightly
    // adjusted speed (positive drift == running behind schedule == speed up;
    // negative == running ahead == slow down) rather than seeking into/
    // skipping content. Returns nullopt when the item has no known duration,
    // or the drift is too large to close within a small, near-imperceptible
    // speed adjustment — callers should fall back to offset-based seeking.
    static std::optional<double> computeSpeed(int64_t rawDriftMs, int64_t durationMs);

public:
    ChannelSession(std::string channel_id, KairosClient& kairos,
                   std::string ffmpeg_path, StreamOptions opts = {});
    ~ChannelSession();

    // Fetches current item from Kairos and starts ffmpeg. Returns false on failure.
    bool start();

    void stop();

    void addClient(std::shared_ptr<ClientSink> sink);
    void removeClient(std::shared_ptr<ClientSink> sink);

    bool isActive() const { return active.load(); }
    const std::string& channelId() const { return channel_id; }
};
