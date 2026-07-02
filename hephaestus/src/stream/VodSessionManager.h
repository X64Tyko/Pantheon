#pragma once
#include "VodSession.h"
#include "../kairos/KairosClient.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Keyed by generated session_id, not channel_id — each VOD viewer gets an
// independent session (unlike SessionManager's per-channel shared sessions).
// A seek or track switch is "stop the old session, create a new one" (see
// the Hephaestus Router's /stream/vod/start handler), so this class is
// intentionally simple: no per-file identity tracking, no reuse.
class VodSessionManager {
public:
    VodSessionManager(std::string ffmpeg_path, VodStreamOptions opts, KairosClient& kairos);
    ~VodSessionManager();

    // Creates and starts a new session. Returns nullptr if start() fails
    // (probe failure, bad file, or ffmpeg wouldn't spawn).
    std::shared_ptr<VodSession> create(const std::string& file_path, int64_t position_ms,
                                        int audio_track, int subtitle_track);
    std::shared_ptr<VodSession> get(const std::string& sessionId);
    void stop(const std::string& sessionId);

private:
    std::string      ffmpeg_path;
    VodStreamOptions opts;
    KairosClient&    kairos;

    std::mutex mtx;
    std::map<std::string, std::shared_ptr<VodSession>> sessions;

    // HLS is poll-based (no persistent connection to react to), so idle
    // sessions are swept periodically rather than torn down on disconnect —
    // same reasoning as ChannelSession's hlsWatchLoop.
    std::atomic<bool> stop_reaper{false};
    std::thread       reaper_thread;
    void reapLoop();

    // Mirrors SessionManager's refreshCache()/kCacheRefreshInterval: applies
    // Kairos-driven settings live (currently verbose_transcode_logs and
    // buffer_size, the two global — not per-channel — settings that also
    // apply to VOD) without needing a Hephaestus restart. VOD has no
    // per-channel config the way live channels do, so there's no channel
    // list to cache here, just these two scalars.
    std::mutex          settings_mtx;
    std::optional<bool> cached_verbose_transcode_logs;
    int                 cached_buffer_size = 0;
    std::atomic<bool>   stop_settings_refresh{false};
    std::thread         settings_refresh_thread;
    void refreshSettings();
};
