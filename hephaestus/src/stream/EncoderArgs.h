#pragma once
#include "ChannelSession.h"     // HwAccel
#include "ClientCapabilities.h" // ClientCapabilities
#include "MediaProbe.h"         // VideoTrack
#include <optional>
#include <set>
#include <string>
#include <vector>

std::string fmtSpeed(double speed);

// Single shared HwAccel-to-string mapping, used by startup log lines
// (HwProbe.cpp, main.cpp) so there's exactly one of these in the codebase.
const char* hwAccelName(HwAccel hw_accel);

// Builds the composite key used in HwCapabilities::decodable_codecs and the
// source_codec argument to pushHwAccelDecodeArgs: the codec name alone for
// 8-bit sources (e.g. "hevc"), or codec name + bit depth for anything higher
// (e.g. "hevc10") — hardware decode support for HEVC/AV1 varies by bit depth
// (older NVDEC/VAAPI generations often decode 8-bit HEVC but not 10-bit), so
// a single "hevc" capable/not-capable answer isn't precise enough. h264 is
// always keyed as plain "h264" regardless of bit_depth — its 10-bit (High
// 10) profile is rare enough in practice not to bother distinguishing.
// The single source of truth for this key: HwProbe.cpp uses it when
// recording which bundled sample clips decoded successfully, and every
// session's spawn path uses it when looking up a real source track's
// bit_depth (MediaProbe's VideoTrack::bit_depth) against that same set —
// keeping both sides on one function is what guarantees they can't drift.
std::string decodeCodecKey(const std::string& codec, int bit_depth);

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
//
// source_video is the probed video track (MediaProbe), or nullptr when
// there's no real source to probe (the offline-slate/splash still-image
// path) or probing failed — either way, treated as "not HDR," matching the
// pre-existing always-tag-bt709 behavior. When it *is* HDR (color_transfer
// is PQ or HLG, see MediaProbe::isHdrTransfer), the output depends on
// client_hdr_capable (see Router.cpp's session-start handlers, sourced from
// the requesting client's own matchMedia('(video-dynamic-range: high)')
// check): false re-tone-maps to genuinely correct SDR-range bt709 (a real
// zscale+tonemap conversion, not the old behavior of just relabeling
// un-tone-mapped PQ-range pixel values as bt709 — that played, but looked
// washed out/hazy on any display); true re-encodes as real HEVC Main10
// HDR10, preserving the source's actual color info instead of downgrading
// it. See EncoderArgs.cpp for the long version of both.
//
// One entry per transcode-target video codec this codebase can actually
// produce, best-to-worst. buildArgs is that codec's own per-HwAccel encode
// args. Exposed (not EncoderArgs.cpp-local) so VodSessionManager can resolve
// the same choice chooseVideoCodec() makes when deciding whether two
// viewers' transcodes can share one encode — see its own comment.
struct VideoCodecOption
{
	std::string name; // ffprobe-style codec name — must match ClientCapabilities::video_codecs entries
	void (*buildArgs)(std::vector<std::string>& a, std::vector<std::string>& vfParts, HwAccel hw_accel);
};

const std::vector<VideoCodecOption>& videoCodecPriority();
// Bounded by the source's own position in the list — never picks something
// more "exotic"/modern than the source already is. See EncoderArgs.cpp for
// the long version.
const VideoCodecOption& chooseVideoCodec(const std::string& source_codec,
										 const std::optional<ClientCapabilities>& client_caps);

// Same idea for audio — bounded by channel count instead of codec
// generation (a surround codec only matters once there's real surround
// content to preserve). See EncoderArgs.cpp.
struct AudioCodecOption
{
	std::string name;
	bool preserve_channels;
	int bitrate_kbps; // 0 = caller's own audio_bitrate_kbps applies instead
};

const std::vector<AudioCodecOption>& audioCodecPriority();
const AudioCodecOption& chooseAudioCodec(const AudioTrack* source_audio,
										 const std::optional<ClientCapabilities>& client_caps);

// client_caps (VOD only — see its own default): "smart muxing" — the
// non-HDR transcode target is chosen via chooseVideoCodec() (EncoderArgs.cpp)
// instead of unconditionally hardcoding H.264, for the same reason
// isVideoDirectStreamable/isAudioDirectStreamable check a *specific* client's own
// declared support rather than a fixed allowlist: a re-encode is sometimes
// unavoidable for a reason that has nothing to do with codec support
// (resolution mismatch, subtitle burn-in, SDR tone-map), and a capable
// client shouldn't lose codec generations just because *something else*
// forced a transcode. Bounded by the source's own codec — see
// chooseVideoCodec's own comment for why re-encoding "up" a generation the
// source never actually had is pointless. Live-channel/preview callers have
// no single-viewer capability to target (a live channel fans one encode out
// to N simultaneous viewers via Hermes — see ChannelBroadcaster) and simply
// don't pass this, keeping their existing fixed-H.264 behavior unchanged.
void pushVideoEncoderArgs(std::vector<std::string>& a, std::vector<std::string>& vfParts,
						  HwAccel hw_accel, int keyframeIntervalSecs,
						  const VideoTrack* source_video                       = nullptr,
						  bool client_hdr_capable                              = false,
						  const std::optional<ClientCapabilities>& client_caps = std::nullopt);

// Audio encoder selection, shared the same way. client_caps/source_audio:
// same "smart muxing" reasoning as pushVideoEncoderArgs' own, via
// chooseAudioCodec() (EncoderArgs.cpp) — a client that's declared real
// eac3/ac3 decode support (native apps/TVs; see the stereo-forcing comment
// in the .cpp for why this never applies to a browser tab) gets its source
// channel layout preserved through a surround-capable codec instead of
// being downmixed to stereo AAC for a browser-MSE limitation that no longer
// applies once nothing here is actually targeting a browser — but only when
// source_audio says there's actually more than stereo to preserve; a
// surround codec buys nothing for an already-stereo source.
// debug_showinfo: appends the `ashowinfo` filter, which logs each audio
// frame's pts_time to stderr — paired with the `showinfo` video filter a
// caller can push into its own vfParts, this gives a directly comparable
// per-stream timestamp trail for diagnosing A/V drift (see
// FfmpegProcess.cpp's stderr timestamp-prefixing, added for the same
// purpose). Off by default: at normal per-frame audio rates this is one line
// per ~20-40ms of audio, far too chatty to leave on outside active
// diagnosis.
void pushAudioEncoderArgs(std::vector<std::string>& a, bool loudnorm, double speed,
						  int audio_bitrate_kbps,
						  const std::optional<ClientCapabilities>& client_caps = std::nullopt,
						  const AudioTrack* source_audio                       = nullptr,
						  bool debug_showinfo                                  = false);

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

// A sane default -maxrate ceiling (kbps) for a live channel's transcode
// bucket when no admin-configured bitrate exists (channel.stream_video_bitrate
// == 0, "quality-based/CRF auto"). Leaving CQ/VBR truly uncapped is a
// documented live-streaming anti-pattern: a complex or forced I-frame can
// spike bitrate arbitrarily, and a real-time (-re-paced) pipeline has no
// slack to absorb that the way file-based encoding would. Generous enough
// that ordinary CQ-23 output for real content should never actually hit
// it — this exists purely to bound the pathological spike case, not to
// meaningfully constrain quality. effective_height <= 0 (unknown, e.g. no
// source_video probed yet) assumes ~1080p-ish.
int defaultBitrateCapKbps(int effective_height);

// The real output height a channel's transcode will land on: the smaller of
// the configured max_resolution cap (resolveMaxHeight() — 0 = uncapped, i.e.
// whatever the source is) and the source's own real height. Needed because
// defaultBitrateCapKbps() has to size itself off what will actually be
// encoded, not just whichever of those two happens to be set.
int effectiveOutputHeight(int max_height_cap, const VideoTrack* source_video);

// Appends "-v verbose" when verbose_transcode_logs is true (see
// Config::verbose_transcode_logs / *StreamOptions::verbose_transcode_logs),
// a no-op otherwise — ffmpeg's own default log level is left untouched when
// the toggle is off, so this never changes existing behavior by itself.
// Call this first, right after pushing the ffmpeg path, so it applies to the
// whole command rather than being scoped to one input/output.
void pushLogLevelArgs(std::vector<std::string>& a, bool verbose_transcode_logs);