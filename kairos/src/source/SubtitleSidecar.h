#pragma once
#include <string>
#include <unordered_map>
#include <vector>

// External subtitle sidecar file detection — source-agnostic (unlike
// SidecarMetadata.h's Kodi .nfo/poster parsing, which is LocalSource-only).
// Plex/Jellyfin/Local files are all read off the same mapped filesystem
// during sync, so a sidecar .srt sitting next to any of them is discoverable
// the same way, same as embedded chapter probing already is.

// One external subtitle file discovered next to a video. file_path is
// whatever the caller's directory listing produced (mapped) — callers that
// need the unmapped, DB-storable path reconstruct it themselves via
// pathutil::parentDir() on the original unmapped video path, since path
// mapping only ever rewrites a directory prefix, never the filename.
struct ExternalSubtitle {
    std::string file_path;
    std::string language;  // ISO 639-2, "" = unknown/und
    bool        forced = false;
    bool        sdh    = false;
    std::string title;     // always "" — no title source in the filename convention
};

// Lists `dir` (a single, already-mapped real directory) exactly once and
// returns every recognised subtitle sidecar file found, keyed by which entry
// of `video_stems` it belongs to — so a caller with many videos in the same
// folder (e.g. a season directory) can list the directory once and look up
// every video's matches, rather than re-listing per video (expensive on
// network-mounted libraries).
//
// video_stems must be the exact filenames (without extension) of the videos
// actually in `dir`, matched against the *longest* stem each candidate file
// is prefixed by — scene-release video filenames routinely contain dots
// themselves ("The.Matrix.1999.1080p"), so a sidecar can't be split into
// "stem" + "tags" by just finding the first dot; it has to be matched
// against a known stem instead (see LocalSource.cpp's parseTitle for the
// same dotted-filename problem elsewhere in this codebase).
//
// Recognises .srt/.ass/.ssa/.vtt only — not .sub, which is ambiguous between
// MicroDVD text and VobSub bitmap-paired-with-.idx (exactly the burn-in case
// external subtitles deliberately don't support). Filename convention:
// "<video_stem>.<lang>?.<forced|sdh|cc>?.<ext>" — e.g. "Movie.eng.srt",
// "Movie.en.forced.srt", "Movie.spa.sdh.ass", or a bare "Movie.srt"
// (language left empty/und). Returns an empty map if `dir` can't be listed.
std::unordered_map<std::string, std::vector<ExternalSubtitle>> scanDirectoryForSubtitles(
    const std::string& dir, const std::vector<std::string>& video_stems);
