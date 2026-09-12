#pragma once
#include <cstdint>
#include <string>

// ffprobe-derived summary used to decide whether a file needs re-encoding —
// same shape/thresholds as Hephaestus's own isLikelyVfr (MediaProbe.cpp
// there), duplicated rather than shared since the two services don't share
// code and this is a small, self-contained check.
struct CodecSummary
{
	bool has_video = false;
	std::string video_codec, audio_codec, r_frame_rate;
	bool likely_vfr     = false;
	int64_t duration_ms = 0;
};

CodecSummary probeCodecSummary(const std::string& file_path);

// video != h264, or audio != aac, or the VFR heuristic tripped. False (skip)
// for a file with no video stream at all — nothing to normalize.
bool needsNormalize(const CodecSummary& summary);

// Re-encodes file_path to H.264/AAC CFR at tmp_path (subtitles/chapters
// copied through). r_frame_rate pins CFR to the source's own nominal rate
// when known (closes the VFR gap), left to ffmpeg's own guess when empty.
// Returns false on a nonzero ffmpeg exit — caller still owns verifying and
// cleaning up tmp_path either way.
bool runNormalizeEncode(const std::string& file_path, const std::string& tmp_path,
						const std::string& r_frame_rate);

// Re-probes tmp_path and confirms it's really h264/aac and, when
// expected_duration_ms > 0, that duration lands within tolerance of it —
// catching a truncated/corrupt encode before it replaces the original.
bool verifyNormalizedOutput(const std::string& tmp_path, int64_t expected_duration_ms);