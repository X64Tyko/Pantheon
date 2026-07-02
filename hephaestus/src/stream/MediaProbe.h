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

// Thread-safe cache in front of probeMedia(), keyed by file_path. Preview
// channel-flips re-probe the same handful of files as viewers cycle through
// channels, and live channel item transitions repeat across playlist loops
// — both are cases where re-running ffprobe on a file whose streams haven't
// changed is wasted work. Entries never expire/evict: probe results don't
// change for a file that isn't itself changing, and cardinality is bounded
// by how many distinct files get probed during this process's uptime.
// Failures are deliberately not cached (a transient issue — file mid-write,
// share hiccup — shouldn't wedge a session into permanent failure).
std::optional<MediaInfo> probeMediaCached(const std::string& ffprobe_path,
                                           const std::string& file_path);

// Returns the relative audio index (for -map 0:a:N) of the best matching
// track: prefers preferred_lang if non-empty, otherwise returns 0.
int pickAudioTrack(const MediaInfo& info, const std::string& preferred_lang);

// Returns the relative subtitle index (for -map 0:s:N) of the best matching
// track, or -1 if not found or preferred_lang is empty.
int pickSubtitleTrack(const MediaInfo& info, const std::string& preferred_lang);
