#pragma once
#include "ChannelSession.h" // HwAccel
#include "FfmpegProcess.h"
#include "../kairos/KairosClient.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

struct PreviewStreamOptions {
    std::string ffprobe_path      = "ffprobe";
    std::string hls_root;
    std::string default_logo_path;
    int         linger_secs       = 60;
    int         buffer_size       = 1048576;
    HwAccel     hw_accel          = HwAccel::none;
    std::string vaapi_device      = "/dev/dri/renderD128";
    bool        ffmpeg_debug_logs = false;
};

// Manifest URL is stable for the session's whole life — switchChannel()
// replaces the underlying ffmpeg process, not the session, so the client
// never remounts its player when the previewed channel changes.
class PreviewSession {
public:
    PreviewSession(std::string session_id, std::string ffmpeg_path,
                   PreviewStreamOptions opts, KairosClient& kairos);
    ~PreviewSession();

    PreviewSession(const PreviewSession&)            = delete;
    PreviewSession& operator=(const PreviewSession&) = delete;

    bool switchChannel(const std::string& channel_id);

    void stop();
    void touch();

    bool isActive() const { return active.load(); }
    bool isIdle() const;
    const std::string& sessionId() const { return session_id; }
    std::string dir() const { return opts.hls_root + "/preview/" + session_id; }

private:
    std::string   session_id;
    std::string   ffmpeg_path;
    PreviewStreamOptions opts;
    KairosClient& kairos;

    std::mutex    ffmpeg_mtx;
    std::unique_ptr<FfmpegProcess> ffmpeg;

    std::atomic<bool>    active{false};
    std::atomic<int64_t> last_touch_ms{0};

    void onExit(int code);
};
