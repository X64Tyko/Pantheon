#include "PreviewSessionManager.h"
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

// Matches SessionManager's kCacheRefreshInterval — see that file's comment
// for why this is a background poll rather than a per-request fetch.
static constexpr auto kSettingsRefreshInterval = std::chrono::seconds(15);

PreviewSessionManager::PreviewSessionManager(std::string ffmpeg_path, PreviewStreamOptions opts, KairosClient& kairos)
    : ffmpeg_path(std::move(ffmpeg_path)), opts(std::move(opts)), kairos(kairos) {
    reaper_thread = std::thread([this] { reapLoop(); });
    refreshSettings(); // blocking, but only once, at startup
    settings_refresh_thread = std::thread([this] {
        while (!stop_settings_refresh.load()) {
            for (int i = 0; i < 15 && !stop_settings_refresh.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (stop_settings_refresh.load()) break;
            refreshSettings();
        }
    });
}

PreviewSessionManager::~PreviewSessionManager() {
    stop_reaper.store(true);
    if (reaper_thread.joinable()) reaper_thread.join();
    stop_settings_refresh.store(true);
    if (settings_refresh_thread.joinable()) settings_refresh_thread.join();
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& [id, s] : sessions) s->stop();
    sessions.clear();
}

void PreviewSessionManager::refreshSettings() {
    auto verbose = kairos.getVerboseTranscodeLogs();
    auto bs      = kairos.getBufferSize();
    std::lock_guard<std::mutex> lock(settings_mtx);
    if (verbose) cached_verbose_transcode_logs = verbose;
    if (bs) cached_buffer_size = *bs * 1024; // KB -> bytes
}

std::shared_ptr<PreviewSession> PreviewSessionManager::create(const std::string& channel_id) {
    PreviewStreamOptions session_opts = opts;
    {
        std::lock_guard<std::mutex> lock(settings_mtx);
        if (cached_verbose_transcode_logs) session_opts.verbose_transcode_logs = *cached_verbose_transcode_logs;
        if (cached_buffer_size > 0) session_opts.buffer_size = cached_buffer_size;
    }

    auto session = std::make_shared<PreviewSession>(generateSessionId(), ffmpeg_path, session_opts, kairos);
    if (!session->switchChannel(channel_id)) return nullptr;

    std::lock_guard<std::mutex> lock(mtx);
    sessions[session->sessionId()] = session;
    return session;
}

std::shared_ptr<PreviewSession> PreviewSessionManager::get(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = sessions.find(sessionId);
    return (it != sessions.end() && it->second->isActive()) ? it->second : nullptr;
}

bool PreviewSessionManager::switchChannel(const std::string& sessionId, const std::string& channel_id) {
    auto session = get(sessionId);
    return session && session->switchChannel(channel_id);
}

void PreviewSessionManager::stop(const std::string& sessionId) {
    std::shared_ptr<PreviewSession> session;
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = sessions.find(sessionId);
        if (it == sessions.end()) return;
        session = it->second;
        sessions.erase(it);
    }
    session->stop();
}

void PreviewSessionManager::reapLoop() {
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
