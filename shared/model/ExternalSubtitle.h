#pragma once
#include <string>

// One external subtitle sidecar file (.srt/.ass/.ssa/.vtt next to a video).
// Shared between Kairos (which catalogs these at sync time — see
// kairos/src/source/SubtitleSidecar.h and the subtitle_track table — and
// returns them from GET /api/playback/:content_type/:id) and Hephaestus
// (which parses that response in kairos/KairosClient.cpp and plays the
// selected file in stream/VodSession.cpp) — one shape, not duplicated per
// layer/service.
struct ExternalSubtitle {
    std::string file_path;
    std::string language; // ISO 639-2, "" = unknown/und
    bool        forced = false;
    bool        sdh    = false;
    std::string title;
};
