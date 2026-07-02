#pragma once
#include "PreviewSession.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class PreviewSessionManager {
public:
    PreviewSessionManager(std::string ffmpeg_path, PreviewStreamOptions opts, KairosClient& kairos);
    ~PreviewSessionManager();

    std::shared_ptr<PreviewSession> create(const std::string& channel_id);
    std::shared_ptr<PreviewSession> get(const std::string& sessionId);
    bool switchChannel(const std::string& sessionId, const std::string& channel_id);
    void stop(const std::string& sessionId);

private:
    std::string          ffmpeg_path;
    PreviewStreamOptions opts;
    KairosClient&        kairos;

    std::mutex mtx;
    std::map<std::string, std::shared_ptr<PreviewSession>> sessions;

    std::atomic<bool> stop_reaper{false};
    std::thread       reaper_thread;
    void reapLoop();

    // Mirrors SessionManager's refreshCache() / VodSessionManager's
    // refreshSettings() — see either for why this is a background poll
    // rather than a per-request fetch. Preview has no per-channel config of
    // its own either, just these two global scalars.
    std::mutex          settings_mtx;
    std::optional<bool> cached_verbose_transcode_logs;
    int                 cached_buffer_size = 0;
    std::atomic<bool>   stop_settings_refresh{false};
    std::thread         settings_refresh_thread;
    void refreshSettings();
};
