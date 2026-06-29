#include "SessionManager.h"
#include <iostream>

SessionManager::SessionManager(KairosClient& kairos, std::string ffmpeg_path, StreamOptions opts)
    : kairos(kairos), ffmpeg_path(std::move(ffmpeg_path)), stream_opts(std::move(opts)) {}

std::shared_ptr<ChannelSession> SessionManager::getOrCreate(const std::string& channelId) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = sessions.find(channelId);
    if (it != sessions.end() && it->second->isActive()) return it->second;

    // Create and start a new session
    auto session = std::make_shared<ChannelSession>(channelId, kairos, ffmpeg_path, stream_opts);
    if (!session->start()) {
        std::cerr << "[sessions] failed to start session for channel " << channelId << "\n";
        return nullptr;
    }

    sessions[channelId] = session;
    return session;
}

void SessionManager::reap() {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto it = sessions.begin(); it != sessions.end(); ) {
        if (!it->second->isActive()) it = sessions.erase(it);
        else ++it;
    }
}
