#pragma once
#include <string>
#include <vector>
#include <optional>

struct AudioTrack {
    int         stream_index = 0;  // ffmpeg stream index within the file
    int         relative_index = 0; // nth audio stream (for -map 0:a:N)
    std::string codec;
    std::string language;          // BCP-47 / ISO 639-2, e.g. "eng"
    std::string title;
    int         channels = 0;
};

struct VideoTrack {
    int         stream_index = 0;
    std::string codec;             // e.g. "h264", "hevc", "av1"
    int         width  = 0;
    int         height = 0;
};

struct SubtitleTrack {
    int         stream_index = 0;
    int         relative_index = 0;
    std::string codec;
    std::string language;
    std::string title;
};

struct MediaInfo {
    std::vector<VideoTrack>    video;
    std::vector<AudioTrack>    audio;
    std::vector<SubtitleTrack> subtitles;
};

// Runs ffprobe on file_path and returns stream info.
// Returns nullopt if ffprobe fails or the file doesn't exist.
std::optional<MediaInfo> probeMedia(const std::string& ffprobe_path,
                                     const std::string& file_path);

// Returns the relative audio index (for -map 0:a:N) of the best matching
// track: prefers preferred_lang if non-empty, otherwise returns 0.
int pickAudioTrack(const MediaInfo& info, const std::string& preferred_lang);
