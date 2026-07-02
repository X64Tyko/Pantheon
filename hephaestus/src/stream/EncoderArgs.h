#pragma once
#include "ChannelSession.h" // HwAccel
#include <string>
#include <vector>

std::string fmtSpeed(double speed);

// NVDEC decode offload — insert right before -i. Deliberately just
// "-hwaccel cuda" without "-hwaccel_output_format cuda": the latter keeps
// frames as CUDA hw surfaces all the way to the encoder, which needs every
// downstream -vf filter to be CUDA-aware (scale_cuda, hwdownload/hwupload
// bridging, etc) and would need real NVENC hardware to verify safely.
// Plain "-hwaccel cuda" only offloads the decode step; ffmpeg copies frames
// back to normal system memory afterward, so every existing filter/pix_fmt
// path (VOD, live, preview) keeps working unmodified. Software decode of a
// 1080p+ HEVC source was measured pegging 4+ CPU cores for minutes without
// ever producing a first HLS segment — NVENC encode itself is fast; decode
// was the actual bottleneck.
void pushHwAccelDecodeArgs(std::vector<std::string>& a, HwAccel hw_accel);

// Video encoder selection, shared across live-channel, offline-slate, and
// VOD ffmpeg argument builders. Appends codec args to `a` and (for AMD,
// which needs a CPU->VAAPI upload after any scale filter) an extra entry to
// `vfParts`.
//
// keyframeIntervalSecs forces a keyframe at that cadence so the HLS muxer can
// actually cut segments near the caller's -hls_time — without this, x264's
// default 250-frame GOP (10s at 25fps) silently overrides any -hls_time
// value, since HLS can only cut segments at keyframes. Must match (or be a
// divisor of) the -hls_time the caller uses for the tee/hls output.
//
// Uses -force_key_frames for software/VAAPI, which libx264 honors reliably.
// NVENC's ffmpeg wrapper does not reliably respect -force_key_frames (it's a
// frame-flagging mechanism aimed at software encoders) — confirmed via a
// real VOD transcode that produced segments indefinitely but never closed
// one cleanly enough for the HLS muxer to ever write playlist.m3u8. NVENC
// gets an explicit -g/-keyint_min GOP size instead, computed from fpsHint
// when known (0 = unknown, falls back to an assumed 30fps so the GOP is
// still bounded rather than left at NVENC's own multi-second default).
void pushVideoEncoderArgs(std::vector<std::string>& a, std::vector<std::string>& vfParts,
                           HwAccel hw_accel, int keyframeIntervalSecs, double fpsHint = 0);

// Audio encoder selection, shared the same way.
void pushAudioEncoderArgs(std::vector<std::string>& a, bool loudnorm, double speed,
                           int audio_bitrate_kbps);

// Joins vfParts with commas and appends "-vf <joined>" to `a` if non-empty.
void pushVideoFilterArgs(std::vector<std::string>& a, const std::vector<std::string>& vfParts);
