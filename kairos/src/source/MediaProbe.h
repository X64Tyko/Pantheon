#pragma once
#include "../model/Chapter.h"
#include <cstdint>
#include <string>
#include <vector>

// Returns a validated duration_ms for a media file.
//
// If dur is already in [1 000, 86 400 000] ms (1 s – 24 h) it is returned
// unchanged.  Otherwise ffprobe is invoked on file_path to obtain the real
// container duration.  If ffprobe succeeds and the result is in range, the
// probed value is returned; if not, 0 is returned and a warning is emitted.
//
// Intended to be called before writing duration_ms to the DB so that stale,
// missing, or corrupt source metadata does not silently produce zero-duration
// items that stall the scheduler.
int64_t validateDurationMs(int64_t dur, const std::string& file_path);

// The same [1 000, 86 400 000] ms range validateDurationMs checks internally,
// exposed so a caller can decide whether a value needs re-probing without
// paying for a probe just to find out. Used by SyncManager's
// syncMediaProbeFromFiles to fold duration re-validation into the same
// combined probeFileInfo() call it needs anyway for resolution/languages,
// rather than a separate ffprobe spawn via validateDurationMs.
bool durationLooksValid(int64_t ms);

// Reads chapter markers embedded in a media file via ffprobe.
// Returns chapters in position order with source="file" and chapter_type="unclassified".
// media_type and media_id are left empty — caller fills them in.
// Returns empty vector if ffprobe fails or the file has no chapters.
std::vector<Chapter> probeChapters(const std::string& file_path);

// Distinct audio and subtitle language codes found in a file (ISO 639-2,
// e.g. "eng", "jpn"; "und" excluded) — populated only via probeFileInfo's
// combined probe below, there's no standalone per-field prober for this one
// (unlike VideoInfo/probeVideoInfo, still probed on-demand by
// ContentService.cpp's /videoinfo endpoint since resolution/codec/bit-depth
// aren't persisted columns the way audio_languages/embedded_subtitle_
// languages are — see Database.cpp's v86 migration comment).
struct StreamLanguages
{
	std::vector<std::string> audio, subtitle;
};

// Video codec/resolution/bit-depth of the first video stream, for display on
// library detail panels (distinct from Hephaestus's own MediaProbe, which
// serves live transcode-time decisions — this is purely informational).
// bit_depth defaults to 8 when ffprobe reports neither bits_per_raw_sample
// nor a pix_fmt with a 10le/12le suffix. Returns a zero-valued struct
// (empty codec, 0x0, bit_depth 8) if ffprobe fails or the file has no video
// stream.
struct VideoInfo
{
	std::string codec;
	int width = 0, height = 0, bit_depth = 8;
};

VideoInfo probeVideoInfo(const std::string& file_path);

// Combined probe: one ffprobe invocation (-show_format -show_streams) for
// everything sync-time file inspection needs — duration (format-level,
// falling back to the longest per-stream duration exactly like
// validateDurationMs's own two-tier fallback), video codec/resolution/bit-
// depth, and audio/subtitle stream languages — instead of separately
// spawning ffprobe for each. validateDurationMs/probeVideoInfo remain
// available as-is for other callers (e.g. ContentService.cpp's on-demand
// /videoinfo endpoint); the language half has no such standalone caller
// anymore — SyncManager::syncMediaProbeFromFiles persists this probe's
// langs into audio_languages/embedded_subtitle_languages, and
// ContentService.cpp's /languages endpoints just read those columns.
// duration_ms is 0 if undeterminable. keyframes_ms is every real keyframe
// timestamp in the primary video stream (packet-level scan, no decode) —
// added here so it rides along with the rest of this sync-time probe instead
// of Hephaestus paying for this same scan again on every direct-stream
// playback start (see Database.cpp's v98 migration comment and
// VodSession.cpp's computeSegmentBoundaries()). Empty if the file has no
// video stream or the scan failed.
struct FileProbeInfo
{
	int64_t duration_ms = 0;
	VideoInfo video;
	StreamLanguages langs;
	std::vector<int64_t> keyframes_ms;
};

FileProbeInfo probeFileInfo(const std::string& file_path);

// Buckets a probed video height into the same "4K"/"1080p"/"720p"/"SD"
// labels the Library filter/sort UI already offers (see RESOLUTIONS in
// hades/src/components/PickerFilters.tsx) — persisted on sync so it's
// filterable without re-probing on every request. Thresholds are set below
// each label's nominal height to tolerate slightly-off encodes (e.g. a
// 1088-line "1080p" mezzanine). Returns "" for height <= 0 (probe failed or
// no video stream), so the column stays empty rather than mislabeled SD.
std::string bucketResolutionLabel(int height);