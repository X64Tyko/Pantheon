#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

struct AudioTrack
{
	int stream_index   = 0; // ffmpeg stream index within the file
	int relative_index = 0; // nth audio stream (for -map 0:a:N)
	std::string codec;
	// ffprobe's AAC profile variant (e.g. "LC", "HE-AAC", "HE-AACv2") — only
	// meaningful for codec=="aac"; distinguishes the mp4a.40.{2,5,29} RFC
	// 6381 object type in VodSession.cpp's audioCodecString. Empty for any
	// other codec.
	std::string profile;
	std::string language; // BCP-47 / ISO 639-2, e.g. "eng"
	std::string title;
	int channels = 0;
};

struct VideoTrack
{
	int stream_index = 0;
	std::string codec; // e.g. "h264", "hevc", "av1"
	// ffprobe's human-readable H.264/HEVC profile name (e.g. "High", "Main")
	// and numeric level*10 (e.g. 40 for level 4.0) — used to derive an RFC
	// 6381 CODECS string for the master playlist (see VodSession.cpp's
	// videoCodecString) so HLS players with strict format-declaration
	// requirements (ExoPlayer) don't have to open/probe every rendition to
	// discover it themselves. Empty/0 if ffprobe didn't report one (some
	// codecs, e.g. av1, don't carry a named profile the same way).
	std::string profile;
	int level     = 0;
	int width     = 0;
	int height    = 0;
	int bit_depth = 8;
	std::string color_transfer;  // e.g. "smpte2084" (PQ), "arib-std-b67" (HLG), "bt709"
	std::string color_primaries; // e.g. "bt2020", "bt709"
	std::string color_space;     // e.g. "bt2020nc", "bt709"
	// ffprobe's container-declared frame rate (e.g. "25/1") vs its actual
	// average derived from frame-count/duration (e.g. "24975/1000") — these
	// diverging is the standard signal for variable frame rate content (see
	// isLikelyVfr below). Raw "num/den" strings, or empty if unreported.
	std::string r_frame_rate;
	std::string avg_frame_rate;
	// "progressive", "tt"/"bb"/"tb"/"bt" (interlaced field order), "unknown",
	// or empty if ffprobe didn't report one.
	std::string field_order;
};

// True for the two HDR transfer functions ffprobe reports (PQ/HDR10(+) and
// HLG) — anything else (bt709, empty/"unknown", gamma22, etc.) is SDR.
bool isHdrTransfer(const std::string& color_transfer);

// True when r_frame_rate and avg_frame_rate diverge by more than simple
// rounding noise — the standard ffprobe-based signal for variable frame rate
// content (duplicate/dropped frames baked into an old telecine/pulldown-era
// encode, common on syndicated TV masters). False (not just "unknown") when
// either field is missing/unparseable — this is a detection aid, not
// something to gate correctness-critical behavior on.
bool isLikelyVfr(const VideoTrack& v);

// Parses ffprobe's "num/den" frame rate strings (e.g. "30000/1001"),
// occasionally a bare integer, or "0/0"/empty when ffprobe had no basis to
// compute one (returns 0.0 in that case, and on any parse failure — never
// throws). Exposed (not MediaProbe.cpp-local) so EncoderArgs.cpp can size
// -g/-r off the same real source frame rate isLikelyVfr already uses.
double parseFrameRateFraction(const std::string& s);

// Internal parser exposed for testing.
int parseBitDepthForTest(const std::string& json_str);

struct SubtitleTrack
{
	int stream_index   = 0;
	int relative_index = 0;
	std::string codec;
	std::string language;
	std::string title;
};

struct MediaInfo
{
	std::vector<VideoTrack> video;
	std::vector<AudioTrack> audio;
	std::vector<SubtitleTrack> subtitles;
	// From ffprobe's format.duration (-show_format) — 0 if ffprobe didn't
	// report one (raw/unusual containers, corrupt headers). Callers needing
	// a duration to precompute an HLS playlist upfront must fall back to
	// another source (e.g. Kairos's own library-scan duration) when this is 0.
	int64_t duration_ms = 0;
};

// Runs ffprobe on file_path and returns stream info.
// Returns nullopt if ffprobe fails or the file doesn't exist.
std::optional<MediaInfo> probeMedia(const std::string& ffprobe_path,
									const std::string& file_path);

// Lists every keyframe's presentation timestamp in the primary video stream,
// in ascending order (always starting at/near 0), by reading packet headers
// only — no decode involved, fast even on long files. A direct-stream (stream
// copy) VOD session can only ever have its HLS segments cut at these exact
// points (ffmpeg's -hls_time is a minimum, not an exact interval: it cuts at
// the first keyframe at or after that many seconds have elapsed since the
// last cut) — VodSession uses this to precompute the REAL segment boundaries
// a direct-stream session will produce, instead of assuming a uniform cadence
// that only actually holds for the transcode paths (where -force_key_frames
// controls keyframe placement directly). Returns an empty vector on probe
// failure or a file with no video stream.
//
// last_packet_ms (optional): the highest pts_time seen across EVERY packet
// scanned (not just keyframes) — a free byproduct of the same single pass,
// since every packet's pts_time is already read to test its keyframe flag.
// probeMedia()'s own format-level duration can come back 0/missing on a
// file whose container metadata is stale or malformed (common on a
// re-muxed/edited source — the moov atom's duration field just wasn't
// updated) even though the packets themselves demux fine; this is a real,
// packet-grounded alternative for exactly that case, filled in whenever the
// pointer is non-null and at least one packet was seen.
std::vector<int64_t> probeKeyframeTimestampsMs(const std::string& ffprobe_path,
											   const std::string& file_path,
											   int64_t* last_packet_ms = nullptr);

// Last-write-time of file_path, as epoch seconds — matches Kairos's own
// SyncManager.cpp::statFingerprint so a cached probe's stashed mtime (e.g.
// PlaybackInfo::keyframes_mtime, KairosNowResponse::keyframes_mtime) can be
// compared against the file's current state. 0 on any stat error, which a
// real file's mtime will practically never equal, so a stat failure here
// just always misses the cache rather than risking a false "unchanged."
int64_t statMtimeEpochSecs(const std::string& file_path);

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