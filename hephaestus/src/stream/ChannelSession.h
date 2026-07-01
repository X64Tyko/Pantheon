#pragma once
#include "../kairos/KairosClient.h"
#include "../kairos/KairosTypes.h"
#include "FfmpegProcess.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>
#include <functional>

// One ClientSink per connected HTTP client. Thread-safe queue the HTTP handler
// thread reads from while the session's reader thread writes to it.
struct ClientSink {
    std::mutex              mtx;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> queue;
    std::atomic<bool>       done{false};
    static constexpr size_t MAX_QUEUE = 64; // ~8 MB at 128 KB chunks; slow clients are dropped
};

enum class HwAccel { none, nvidia, amd };

struct StreamOptions {
    std::string ffprobe_path      = "ffprobe";
    std::string audio_lang        = "eng";
    std::string subtitle_lang     = "";   // empty = no subtitle mapping
    bool        loudnorm          = false;
    int         linger_secs       = 60;
	int		buffer_size		  = 1048576; // 1024 KB
    HwAccel     hw_accel          = HwAccel::none;
    std::string vaapi_device      = "/dev/dri/renderD128";
    bool        ffmpeg_debug_logs = false; // pipe ffmpeg stderr into the log stream
};

class ChannelSession {
    std::string   channel_id;  // Kairos channel UUID
    KairosClient& kairos;
    std::string   ffmpeg_path;
    StreamOptions opts;

    std::mutex   clients_mtx;
    std::vector<std::shared_ptr<ClientSink>> clients;
    std::atomic<int>  client_count{0};
    std::atomic<int>  stop_token{0};       // incremented on each scheduleStop call

    std::mutex   ffmpeg_mtx;
    std::unique_ptr<FfmpegProcess> ffmpeg;

    KairosNowResponse current_item;
    std::chrono::steady_clock::time_point item_start;

    std::atomic<bool> active{false};

    void onData(const uint8_t* data, size_t len);
    void onExit(int code);
    void transition();
    void spawnFfmpeg(const KairosNowResponse& item, int64_t startOffsetMs);
    void broadcastDone();
    void scheduleStop();

public:
    ChannelSession(std::string channel_id, KairosClient& kairos,
                   std::string ffmpeg_path, StreamOptions opts = {});
    ~ChannelSession();

    // Fetches current item from Kairos and starts ffmpeg. Returns false on failure.
    bool start();

    void stop();

    void addClient(std::shared_ptr<ClientSink> sink);
    void removeClient(std::shared_ptr<ClientSink> sink);

    bool isActive() const { return active.load(); }
    const std::string& channelId() const { return channel_id; }
};
