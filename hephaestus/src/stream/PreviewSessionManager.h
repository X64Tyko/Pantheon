#pragma once
#include "PreviewSession.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
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
};
