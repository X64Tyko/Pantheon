#include "EncoderArgs.h"
#include <sstream>
#include <iomanip>

std::string fmtSpeed(double speed) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << speed;
    return ss.str();
}

const char* hwAccelName(HwAccel hw_accel) {
    switch (hw_accel) {
        case HwAccel::nvidia: return "nvidia";
        case HwAccel::amd:    return "amd";
        default:              return "none";
    }
}

std::string decodeCodecKey(const std::string& codec, int bit_depth) {
    if (codec == "h264" || bit_depth <= 8) return codec;
    return codec + std::to_string(bit_depth);
}

void pushVaapiDeviceArg(std::vector<std::string>& a, HwAccel encode, HwAccel decode,
                         const std::string& vaapi_device) {
    if (encode == HwAccel::amd || decode == HwAccel::amd)
        a.insert(a.end(), {"-vaapi_device", vaapi_device});
}

void pushHwAccelDecodeArgs(std::vector<std::string>& a, HwAccel decode_backend,
                            const std::set<std::string>& decodable_codecs,
                            const std::string& source_codec) {
    if (decode_backend == HwAccel::none) return;
    if (source_codec.empty() || !decodable_codecs.count(source_codec)) return;
    if (decode_backend == HwAccel::nvidia) a.insert(a.end(), {"-hwaccel", "cuda"});
    else if (decode_backend == HwAccel::amd) a.insert(a.end(), {"-hwaccel", "vaapi"});
}

void pushVideoEncoderArgs(std::vector<std::string>& a, std::vector<std::string>& vfParts,
                           HwAccel hw_accel, int keyframeIntervalSecs) {
    switch (hw_accel) {
        case HwAccel::nvidia:
            a.insert(a.end(), {"-c:v", "h264_nvenc", "-preset", "p4",
                                "-rc:v", "vbr", "-cq", "23", "-pix_fmt", "yuv420p",
                                // Without this, h264_nvenc can silently treat a
                                // -force_key_frames request as an ordinary frame
                                // its own internal GOP logic is still free to
                                // reorder/skip, falling back to its default
                                // ~250-frame GOP instead of the caller's
                                // keyframeIntervalSecs -- HLS segments then land
                                // at that default interval (e.g. 10.4s at 24fps)
                                // instead of near -hls_time. Confirmed against a
                                // real VOD session: #EXTINF was landing at
                                // exactly 250 frames' worth of playback time.
                                // -forced-idr makes NVENC emit a true IDR frame
                                // for every forced keyframe instead.
                                "-forced-idr", "1"});
            break;
        case HwAccel::amd:
            vfParts.push_back("format=nv12,hwupload");
            a.insert(a.end(), {"-c:v", "h264_vaapi"});
            break;
        default:
            a.insert(a.end(), {"-c:v", "libx264", "-preset", "veryfast",
                                "-crf", "23", "-pix_fmt", "yuv420p"});
    }
    a.insert(a.end(), {"-force_key_frames",
                        "expr:gte(t,n_forced*" + std::to_string(keyframeIntervalSecs) + ")"});

    // This pipeline never does real HDR tone-mapping -- every branch above
    // targets plain 8-bit yuv420p output regardless of source. For an HDR
    // (BT.2020/PQ) source, ffmpeg's automatic pix_fmt conversion correctly
    // truncates the bit depth but leaves the *color tags* copied through
    // from the input, producing output that's structurally SDR (yuv420p,
    // naively truncated values) while its VUI metadata still claims
    // BT.2020/SMPTE ST 2084 (PQ) HDR. Confirmed via ffprobe against a real
    // transcoded segment: pix_fmt=yuv420p but color_transfer=smpte2084,
    // color_primaries=bt2020, color_space=bt2020nc. Browsers that respect
    // embedded HDR signaling (common on hardware-accelerated decode paths)
    // can silently fail to render that mismatch -- no fatal error surfaces
    // to hls.js/JS, playback just never starts. Explicitly retagging the
    // output as bt709 makes the declared color space match what the pixel
    // data actually is (un-tone-mapped SDR-range values), which is what
    // actually matters for playback; it doesn't make a naively-truncated HDR source
    // look *correct* (that needs real tone-mapping, e.g. zscale+tonemap,
    // future work if visual quality on HDR sources matters), only playable.
    a.insert(a.end(), {"-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709"});
}

void pushAudioEncoderArgs(std::vector<std::string>& a, bool loudnorm, double speed,
                           int audio_bitrate_kbps) {
    std::vector<std::string> afParts;
    // dynaudnorm, not loudnorm: loudnorm's single-pass mode is a real-time
    // estimate with no knowledge of the whole stream, and ffmpeg's own docs
    // note it "shouldn't be used for VOD" for that reason (see VodSession's
    // loudnorm=false) — the same instability shows up as audible drift over
    // a channel's continuous, hours-long runtime. dynaudnorm re-normalizes
    // on short (<8s) windows instead of one long-running estimate, which is
    // what ffmpeg recommends for exactly this always-on streaming case.
    if (loudnorm) afParts.push_back("dynaudnorm");
    if (speed != 1.0) afParts.push_back("atempo=" + fmtSpeed(speed));

    // Force stereo: without an explicit -ac, ffmpeg preserves the source's
    // channel count (e.g. a 5.1/7.1 theatrical mix), and browsers' MSE
    // SourceBuffer support for multichannel AAC is far less reliable than
    // for stereo -- if the audio SourceBuffer never successfully
    // initializes, playback can stall entirely even though video decodes
    // fine (MSE generally wants every active SourceBuffer ready before
    // playback starts). Nothing watching through a browser tab does true
    // surround passthrough anyway, so downmixing here costs nothing real.
    a.insert(a.end(), {"-c:a", "aac", "-ac", "2", "-b:a", std::to_string(audio_bitrate_kbps) + "k"});
    if (!afParts.empty()) {
        std::string af;
        for (size_t i = 0; i < afParts.size(); ++i) { if (i) af += ","; af += afParts[i]; }
        a.insert(a.end(), {"-af", af});
    }
}

void pushVideoFilterArgs(std::vector<std::string>& a, const std::vector<std::string>& vfParts) {
    if (vfParts.empty()) return;
    std::string vf;
    for (size_t i = 0; i < vfParts.size(); ++i) { if (i) vf += ","; vf += vfParts[i]; }
    a.insert(a.end(), {"-vf", vf});
}

int resolveMaxHeight(const std::string& max_resolution) {
    if (max_resolution == "1080p") return 1080;
    if (max_resolution == "720p")  return 720;
    if (max_resolution == "480p")  return 480;
    return 0; // "source" or unrecognized
}

void pushScaleFilter(std::vector<std::string>& vfParts, int maxHeight) {
    if (maxHeight <= 0) return;
    vfParts.push_back("scale=-2:min(ih\\," + std::to_string(maxHeight) + ")");
}

void pushBitrateCapArgs(std::vector<std::string>& a, int video_bitrate_kbps) {
    if (video_bitrate_kbps <= 0) return;
    std::string maxrate = std::to_string(video_bitrate_kbps) + "k";
    std::string bufsize = std::to_string(video_bitrate_kbps * 2) + "k";
    a.insert(a.end(), {"-maxrate", maxrate, "-bufsize", bufsize});
}

void pushLogLevelArgs(std::vector<std::string>& a, bool verbose_transcode_logs) {
    if (verbose_transcode_logs) a.insert(a.end(), {"-v", "verbose"});
}
