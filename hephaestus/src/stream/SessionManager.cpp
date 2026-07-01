#include "SessionManager.h"
#include <iostream>

SessionManager::SessionManager(KairosClient& kairos, std::string ffmpeg_path, StreamOptions opts)
    : kairos(kairos), ffmpeg_path(std::move(ffmpeg_path)), stream_opts(std::move(opts)) {}

std::shared_ptr<ChannelSession> SessionManager::getOrCreate(const std::string& channelId) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = sessions.find(channelId);
    if (it != sessions.end() && it->second->isActive()) return it->second;

    // Apply per-channel language overrides and transcode quality from Kairos channel config
    StreamOptions opts = stream_opts;
    for (const auto& ch : kairos.getChannels()) {
        if (ch.channel_id == channelId) {
            if (!ch.audio_lang.empty())    opts.audio_lang    = ch.audio_lang;
            if (!ch.subtitle_lang.empty()) opts.subtitle_lang = ch.subtitle_lang;
            opts.max_resolution      = ch.stream_resolution;
            opts.video_bitrate_kbps  = ch.stream_video_bitrate;
            opts.audio_bitrate_kbps  = ch.stream_audio_bitrate > 0 ? ch.stream_audio_bitrate : 192;
            break;
        }
    }

    // Pick up any live changes to the buffer size setting from Kairos so new
    // sessions reflect the latest value without requiring a Hephaestus restart.
    // Falls back to the local/startup default if Kairos is unreachable.
    if (auto bs = kairos.getBufferSize()) opts.buffer_size = *bs * 1024; // KB -> bytes

    // Create and start a new session
    auto session = std::make_shared<ChannelSession>(channelId, kairos, ffmpeg_path, opts);
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
