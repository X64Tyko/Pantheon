#pragma once
#include "ChannelSession.h" // HwAccel
#include <set>
#include <string>
#include <vector>

std::string fmtSpeed(double speed);

// Single shared HwAccel-to-string mapping, used by startup log lines
// (HwProbe.cpp, main.cpp) so there's exactly one of these in the codebase.
const char* hwAccelName(HwAccel hw_accel);

// AMD VAAPI: exposes the render node before -i so the encoder/decoder can
// find it. Insert if *either* encode or decode resolved to amd — VAAPI
// decode offload needs the device set up globally the same way encode does.
// No-op otherwise.
void pushVaapiDeviceArg(std::vector<std::string>& a, HwAccel encode, HwAccel decode,
                         const std::string& vaapi_device);

// NVDEC/VAAPI decode offload — insert right before -i. Deliberately just
// "-hwaccel cuda"/"-hwaccel vaapi" without an explicit hwaccel_output_format:
// the latter keeps frames as hw surfaces all the way to the encoder, which
// needs every downstream -vf filter to be hwaccel-aware (scale_cuda,
// hwdownload/hwupload bridging, etc) and would need real hardware to verify
// safely. Plain "-hwaccel <backend>" only offloads the decode step; ffmpeg
// copies frames back to normal system memory afterward, so every existing
// filter/pix_fmt path (VOD, live, preview) keeps working unmodified.
// Software decode of a 1080p+ HEVC source was measured pegging 4+ CPU cores
// for minutes without ever producing a first HLS segment — encode itself is
// fast; decode was the actual bottleneck.
//
// decode_backend and decodable_codecs come from HwProbe::probeHwCapabilities()
// (HwCapabilities::decode / ::decodable_codecs), resolved once at startup and
// threaded through *StreamOptions::decode_hw_accel / ::decodable_codecs.
// decode_backend is resolved independently of the encode backend — a session
// can legitimately end up GPU-decode+CPU-encode or CPU-decode+GPU-encode,
// not just all-hw/all-sw (this mirrors what AMD's encode path already does
// unconditionally: CPU decode + GPU encode). source_codec is this item's
// ffprobe-reported video codec ("h264"/"hevc"/"av1"), or empty when
// unknown/unprobed. The flag is only appended when decode_backend != none
// AND source_codec is non-empty AND present in decodable_codecs — an
// unprobed source, an untested codec, or vp9 (deliberately never probed,
// it's a web/YouTube codec not relevant to a movie/TV library) all fall
// through to ordinary CPU decode.
void pushHwAccelDecodeArgs(std::vector<std::string>& a, HwAccel decode_backend,
                            const std::set<std::string>& decodable_codecs,
                            const std::string& source_codec);

// Video encoder selection, shared across live-channel, offline-slate, and
// VOD ffmpeg argument builders. Appends codec args to `a` and (for AMD,
// which needs a CPU->VAAPI upload after any scale filter) an extra entry to
// `vfParts`.
//
// keyframeIntervalSecs forces a keyframe at that cadence (via
// -force_key_frames, framerate-independent unlike -g) so the HLS muxer can
// actually cut segments near the caller's -hls_time — without this, x264's
// default 250-frame GOP (10s at 25fps) silently overrides any -hls_time
// value, since HLS can only cut segments at keyframes. Must match (or be a
// divisor of) the -hls_time the caller uses for the tee/hls output.
void pushVideoEncoderArgs(std::vector<std::string>& a, std::vector<std::string>& vfParts,
                           HwAccel hw_accel, int keyframeIntervalSecs);

// Audio encoder selection, shared the same way.
void pushAudioEncoderArgs(std::vector<std::string>& a, bool loudnorm, double speed,
                           int audio_bitrate_kbps);

// Joins vfParts with commas and appends "-vf <joined>" to `a` if non-empty.
void pushVideoFilterArgs(std::vector<std::string>& a, const std::vector<std::string>& vfParts);

// Maps the "source"|"1080p"|"720p"|"480p" per-channel/VOD quality setting to
// a max output height in pixels, or 0 for "source"/unrecognized (no scaling).
int resolveMaxHeight(const std::string& max_resolution);

// Appends "scale=-2:min(ih,maxHeight)" to vfParts if maxHeight > 0 (never
// upscales; 0 = no scale filter at all). The one canonical implementation of
// this filter string — callers pass either resolveMaxHeight()'s result or a
// fixed height constant (e.g. preview's capped-height thumbnail stream).
void pushScaleFilter(std::vector<std::string>& vfParts, int maxHeight);

// Appends -maxrate/-bufsize (bufsize = 2x maxrate) if video_bitrate_kbps > 0,
// a no-op otherwise.
void pushBitrateCapArgs(std::vector<std::string>& a, int video_bitrate_kbps);

// Appends "-v verbose" when verbose_transcode_logs is true (see
// Config::verbose_transcode_logs / *StreamOptions::verbose_transcode_logs),
// a no-op otherwise — ffmpeg's own default log level is left untouched when
// the toggle is off, so this never changes existing behavior by itself.
// Call this first, right after pushing the ffmpeg path, so it applies to the
// whole command rather than being scoped to one input/output.
void pushLogLevelArgs(std::vector<std::string>& a, bool verbose_transcode_logs);
