#include "VodSessionManager.h"
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

static std::string generateSessionId() {
    thread_local std::mt19937_64 rng(std::random_device{}());
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << rng();
    return ss.str();
}

VodSessionManager::VodSessionManager(std::string ffmpeg_path, VodStreamOptions opts)
    : ffmpeg_path(std::move(ffmpeg_path)), opts(std::move(opts)) {
    reaper_thread = std::thread([this] { reapLoop(); });
}

VodSessionManager::~VodSessionManager() {
    stop_reaper.store(true);
    if (reaper_thread.joinable()) reaper_thread.join();
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& [id, s] : sessions) s->stop();
    sessions.clear();
}

std::shared_ptr<VodSession> VodSessionManager::create(const std::string& file_path, int64_t position_ms,
                                                        int audio_track, int subtitle_track) {
    auto session = std::make_shared<VodSession>(generateSessionId(), ffmpeg_path, opts);
    if (!session->start(file_path, position_ms, audio_track, subtitle_track)) return nullptr;

    std::lock_guard<std::mutex> lock(mtx);
    sessions[session->sessionId()] = session;
    return session;
}

std::shared_ptr<VodSession> VodSessionManager::get(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = sessions.find(sessionId);
    return (it != sessions.end() && it->second->isActive()) ? it->second : nullptr;
}

void VodSessionManager::stop(const std::string& sessionId) {
    std::shared_ptr<VodSession> session;
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = sessions.find(sessionId);
        if (it == sessions.end()) return;
        session = it->second;
        sessions.erase(it);
    }
    session->stop();
}

void VodSessionManager::reapLoop() {
    while (!stop_reaper.load()) {
        for (int i = 0; i < 5 && !stop_reaper.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (stop_reaper.load()) break;

        std::lock_guard<std::mutex> lock(mtx);
        for (auto it = sessions.begin(); it != sessions.end(); ) {
            if (!it->second->isActive() || it->second->isIdle()) {
                it->second->stop();
                it = sessions.erase(it);
            } else ++it;
        }
    }
}
