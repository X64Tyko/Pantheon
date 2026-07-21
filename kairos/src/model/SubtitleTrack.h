#pragma once
#include <string>

// External subtitle sidecar file (.srt/.ass/.ssa/.vtt next to a video) —
// see SubtitleSidecar.h for detection and SyncManager's
// syncSubtitleTracksFromFiles for how these get written. Embedded subtitle
// streams are not represented here — those stay ephemeral/probe-time-only
// in Hephaestus's own MediaProbe, same as before this table existed.
struct SubtitleTrack {
    std::string subtitle_id;
    std::string media_type;   // "episode" | "movie"
    std::string media_id;
    std::string file_path;    // unmapped, like episode/movie.file_path
    std::string language;     // ISO 639-2, "" = unknown/und
    bool        forced = false;
    bool        sdh    = false;
    std::string title;
    std::string source = "file";
};
