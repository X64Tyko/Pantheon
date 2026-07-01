#pragma once
#include <string>

struct Channel {
    std::string channel_id;
    std::string name;
    int         number                   = 0;
    std::string timezone                 = "UTC";
    std::string advance_mode             = "scheduled";
    std::string default_filler_selection = "round_robin";
    int         seed                     = 12345;
    std::string offline_video_path;
    std::string offline_image_path;
    std::string offline_audio_id;
    std::string offline_audio_type;
    std::string offline_audio_title;
    std::string logo_path;
    std::string anchor_hashes;  // JSON blob, may be empty
    std::string audio_lang;     // preferred audio language, e.g. "eng"; empty = default
    std::string subtitle_lang;  // preferred subtitle language; empty = none
    // Per-channel transcode quality settings (Hephaestus)
    std::string stream_resolution    = "source"; // "source"|"1080p"|"720p"|"480p"
    int         stream_video_bitrate = 0;        // kbps; 0 = CRF/CQ auto
    int         stream_audio_bitrate = 192;      // kbps
};
