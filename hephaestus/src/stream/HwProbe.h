#pragma once
#include "ChannelSession.h" // HwAccel
#include <set>
#include <string>

// Resolved, verified GPU capability for this process's entire lifetime.
// Computed once in main() before any SessionManager/VodSessionManager/
// PreviewSessionManager exists, then copied into StreamOptions/
// VodStreamOptions/PreviewStreamOptions. Every session downstream reads
// these pre-validated facts instead of blindly trusting Config::hw_accel —
// there is no per-request retry/fallback path anywhere in this codebase, so
// getting it right once at startup is the only chance.
struct HwCapabilities {
    // Resolved encode backend -- feeds pushVideoEncoderArgs unchanged.
    // Independent of `decode` below: an encode failure (e.g. an NVENC
    // session-count cap) no longer forces software decode too, and a decode
    // failure (e.g. an unsupported profile) no longer forces software
    // encode -- each falls back independently. This mirrors what AMD's
    // encode path already does unconditionally today (CPU decode + GPU
    // encode) -- proof the mixed mode works fine in practice.
    HwAccel encode = HwAccel::none;

    // Resolved decode backend -- tells pushHwAccelDecodeArgs which flag to
    // emit ("-hwaccel cuda" for nvidia, "-hwaccel vaapi" for amd).
    // HwAccel::none if the device node wasn't accessible at all. Resolved
    // independently of `encode` above.
    HwAccel decode = HwAccel::none;

    // Source codecs (ffprobe codec_name: "h264"/"hevc"/"av1") whose hwaccel
    // decode smoke-test passed against `decode`'s backend. Empty unless
    // `decode != none`.
    std::set<std::string> decodable_codecs;
};

// Two-tier, synchronous, blocking startup probe. Safe to call unconditionally
// (including requested == HwAccel::none, which returns immediately with no
// subprocess spawned). assets_dir is where the bundled probe_h264.mp4/
// probe_hevc.mp4/probe_hevc_10bit.mp4/probe_av1.mp4/probe_av1_10bit.mp4
// decode samples live (see Dockerfile); a missing sample degrades to
// skipping just that codec's decode probe, never a hard failure.
//
// verbose_transcode_logs (Config::verbose_transcode_logs) additionally
// prints a final summary enumerating every resolved (codec, bit depth)
// combination this backend can hwaccel-decode, on top of the per-test
// OK/FAILED lines that always print regardless of this flag.
HwCapabilities probeHwCapabilities(HwAccel requested, const std::string& ffmpeg_path,
                                    const std::string& vaapi_device, const std::string& assets_dir,
                                    bool verbose_transcode_logs);
