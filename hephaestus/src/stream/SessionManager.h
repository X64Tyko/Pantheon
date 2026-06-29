#pragma once
#include "ChannelSession.h"
#include "../kairos/KairosClient.h"
#include <string>
#include <map>
#include <memory>
#include <mutex>

class SessionManager {
    KairosClient&  kairos;
    std::string    ffmpeg_path;
    StreamOptions  stream_opts;

    std::mutex     mtx;
    std::map<std::string, std::shared_ptr<ChannelSession>> sessions;

public:
    SessionManager(KairosClient& kairos, std::string ffmpeg_path, StreamOptions opts);

    // Returns an active session for channelId, creating and starting one if needed.
    // Returns nullptr if Kairos rejects the channel or ffmpeg won't start.
    std::shared_ptr<ChannelSession> getOrCreate(const std::string& channelId);

    // Remove sessions that have been stopped.
    void reap();
};
