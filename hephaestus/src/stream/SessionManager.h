#pragma once
#include "ChannelSession.h"
#include "../kairos/KairosClient.h"
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <atomic>
#include <thread>

class SessionManager {
    KairosClient&  kairos;
    std::string    ffmpeg_path;
    StreamOptions  stream_opts;

    std::mutex     mtx;
    std::map<std::string, std::shared_ptr<ChannelSession>> sessions;

    // Channel list / stream buffer size, refreshed periodically in the
    // background instead of fetched from Kairos on every getOrCreate() call —
    // see the comment on kCacheRefreshInterval in SessionManager.cpp for why.
    std::mutex                 cache_mtx;
    std::vector<KairosChannel> cached_channels;
    int                        cached_buffer_size = 0;
    // nullopt = not yet fetched (or Kairos unreachable) — keep whatever
    // Config/--verbose-transcode/HEPH_VERBOSE_TRANSCODE set at startup.
    std::optional<bool>        cached_verbose_transcode_logs;
    std::atomic<bool>          stop_refresh{false};
    std::thread                refresh_thread;

    void refreshCache();

public:
    SessionManager(KairosClient& kairos, std::string ffmpeg_path, StreamOptions opts);
    ~SessionManager();

    // Returns an active session for channelId, creating and starting one if needed.
    // Returns nullptr if Kairos rejects the channel or ffmpeg won't start.
    std::shared_ptr<ChannelSession> getOrCreate(const std::string& channelId);

    // Remove sessions that have been stopped.
    void reap();

    // Snapshot of currently-active sessions, for the activity/debugging view
    // (ActivityRouter). Copies the shared_ptrs out under the lock rather
    // than returning a reference, so the caller can inspect them without
    // holding mtx.
    std::vector<std::shared_ptr<ChannelSession>> listActive();
};
