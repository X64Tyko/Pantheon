#pragma once
#include <string>
#include <optional>
#include <cstdint>

struct KairosNowResponse {
    std::string item_type;
    std::string item_id;
    std::string file_path;
    std::string title;
    std::string block_id;
    int64_t     duration_ms          = 0;
    int64_t     wall_clock_start_ms  = 0;
    int64_t     wall_clock_end_ms    = 0;
    bool        is_filler            = false;
    std::optional<std::string> show_title;
    std::optional<std::string> show_id;
    std::optional<std::string> source_id;
    std::optional<std::string> external_id;
    std::optional<int> season;
    std::optional<int> episode_num;
    std::optional<std::string> offline_image_path;
    std::optional<std::string> offline_audio_path;
};

struct KairosChannel {
    std::string channel_id;
    std::string name;
    int         number = 0;
    std::string audio_lang;    // overrides global --audio-lang when non-empty
    std::string subtitle_lang; // empty = no subtitle mapping
    std::string logo_path;     // empty = no channel-specific logo configured
    // Per-channel transcode quality (mirrors channel.stream_* DB columns)
    std::string stream_resolution    = "source"; // "source"|"1080p"|"720p"|"480p"
    int         stream_video_bitrate = 0;        // kbps; 0 = CRF/CQ auto
    int         stream_audio_bitrate = 192;      // kbps
};
