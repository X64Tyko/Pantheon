#pragma once
#include "ChannelSession.h" // HwAccel
#include "FfmpegProcess.h"
#include "MediaProbe.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

struct VodStreamOptions {
    std::string ffprobe_path = "ffprobe";
    // VOD sessions live at "<hls_root>/vod/<session_id>/". Never empty in
    // practice — VOD has no non-HLS fallback the way live channels do.
    std::string hls_root;
    int         linger_secs       = 60;
    int         buffer_size       = 1048576;
    HwAccel     hw_accel          = HwAccel::none;
    std::string vaapi_device      = "/dev/dri/renderD128";
    bool        ffmpeg_debug_logs = false;
};

// One file, one viewer. Unlike ChannelSession there's no scheduling and no
// client fan-out — ffmpeg writes HLS segments straight to disk and the HTTP
// layer serves them as static files. "Seek" and "switch track" are both
// implemented by the caller tearing down the session and calling start()
// again at the new position/track — see VodSessionManager and the Hephaestus
// Router's /stream/vod/start handler.
class VodSession {
public:
    VodSession(std::string session_id, std::string ffmpeg_path, VodStreamOptions opts);
    ~VodSession();

    VodSession(const VodSession&)            = delete;
    VodSession& operator=(const VodSession&) = delete;

    // Probes file_path, decides direct-play vs transcode, and spawns ffmpeg
    // writing an HLS VOD playlist to dir(). Returns false if probing fails
    // (file missing/unreadable) or ffmpeg won't start.
    bool start(const std::string& file_path, int64_t position_ms,
               int audio_track, int subtitle_track);

    void stop();
    // Called by the HTTP handler on every playlist/segment GET.
    void touch();

    bool isActive() const { return active.load(); }
    bool isIdle() const;
    const std::string& sessionId() const { return session_id; }
    std::string dir() const { return opts.hls_root + "/vod/" + session_id; }
    bool directPlay() const { return direct_play; }
    const MediaInfo& tracks() const { return media_info; }
    bool hasSubtitleOutput() const { return subtitle_output; }

private:
    std::string   session_id;
    std::string   ffmpeg_path;
    VodStreamOptions opts;

    std::mutex    ffmpeg_mtx;
    std::unique_ptr<FfmpegProcess> ffmpeg;

    std::atomic<bool>    active{false};
    std::atomic<int64_t> last_touch_ms{0};
    bool          direct_play     = false;
    bool          subtitle_output = false;
    MediaInfo     media_info;

    void onExit(int code);
};
