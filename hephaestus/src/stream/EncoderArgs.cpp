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

// ffprobe and zscale/ffmpeg's output color options share the same enum
// names (both come from ffmpeg's own AVColor* enums), so a probed value can
// be passed straight through to either without translation. Falls back to
// the overwhelmingly common HDR10 triple (bt2020/bt2020nc/smpte2084) for any
// field ffprobe didn't report, rather than leaving it unset — zscale in
// particular fails outright ("no path between colorspaces") given a
// half-specified input colorspace instead of guessing.
std::string orDefaultColorTag(const std::string& v, const char* def) {
    return v.empty() ? def : v;
}

// Real HDR (PQ/HLG, BT.2020) -> SDR (BT.709) conversion, not just a metadata
// relabel. Verified end-to-end against a real ffmpeg n8.1.1 build (libzimg):
// zscale's auto-detection of embedded frame colorspace is unreliable enough
// on real-world files (partial/missing tags, common on older or badly-muxed
// rips) that leaving transferin/primariesin/matrixin unset intermittently
// fails with "no path between colorspaces" — passing the values MediaProbe
// already read from ffprobe explicitly makes this deterministic regardless
// of how complete the source's own tagging is.
//
// hable+desat=0 (no automatic desaturation of bright highlights) is a
// deliberately plain, content-agnostic tone-map operator — no per-title
// tuning here, this pipeline has no concept of per-file mastering metadata
// beyond the VUI tags themselves.
std::string buildHdrToSdrTonemapFilter(const VideoTrack& source_video) {
    std::string transfer  = orDefaultColorTag(source_video.color_transfer,  "smpte2084");
    std::string primaries = orDefaultColorTag(source_video.color_primaries, "bt2020");
    std::string matrix    = orDefaultColorTag(source_video.color_space,     "bt2020nc");

    return "zscale=transferin=" + transfer + ":primariesin=" + primaries + ":matrixin=" + matrix +
           ":transfer=linear:primaries=" + primaries + ":matrix=" + matrix + ":npl=100"
           ",format=gbrpf32le"
           ",zscale=primaries=bt709"
           ",tonemap=tonemap=hable:desat=0"
           ",zscale=transfer=bt709:matrix=bt709:primaries=bt709:range=tv"
           ",format=yuv420p";
}

void pushVideoEncoderArgs(std::vector<std::string>& a, std::vector<std::string>& vfParts,
                           HwAccel hw_accel, int keyframeIntervalSecs,
                           const VideoTrack* source_video, bool client_hdr_capable) {
    bool source_hdr      = source_video && isHdrTransfer(source_video->color_transfer);
    bool passthrough_hdr = source_hdr && client_hdr_capable;

    if (source_hdr && !passthrough_hdr)
        vfParts.insert(vfParts.begin(), buildHdrToSdrTonemapFilter(*source_video));

    // Real HDR10 passthrough: re-encode at true 10-bit instead of downgrading
    // to 8-bit/BT.709, for a client that already told us (via
    // client_hdr_capable — see Router.cpp's session-start handlers) its
    // display can actually show the extra range. H.264 has no real-world
    // HDR10 story, so this is HEVC Main10 across every backend, verified
    // against a real ffmpeg n8.1.1 build (libx265: Main 10 profile,
    // yuv420p10le, color_space round-tripped correctly through an .m3u8/.ts
    // HLS segment). NVENC/VAAPI 10-bit paths follow the same shape as their
    // existing 8-bit branches below but couldn't be verified end-to-end here
    // (no GPU in this environment) — pix_fmt choice per backend (p010le for
    // hardware surfaces vs yuv420p10le for libx265) matches each encoder's
    // own supported-pixel-format list (`ffmpeg -h encoder=<name>`).
    //
    // Deliberately does NOT attempt to carry over the source's static HDR10
    // metadata (Mastering Display Color Volume / MaxCLL/MaxFALL SEI) — real
    // benefit (a receiver's own tone-mapping tuned to the actual mastering
    // range) but meaningfully more work to extract via ffprobe and re-inject
    // via -master_display/-max_cll, and most receivers fall back to sane
    // generic assumptions without it. Follow-up if displays look off without it.
    if (passthrough_hdr) {
        switch (hw_accel) {
            case HwAccel::nvidia:
                a.insert(a.end(), {"-c:v", "hevc_nvenc", "-preset", "p4", "-profile:v", "main10",
                                    "-rc:v", "vbr", "-cq", "23", "-pix_fmt", "p010le",
                                    "-forced-idr", "1"}); // see the 8-bit nvenc branch below for why
                break;
            case HwAccel::amd:
                vfParts.push_back("format=p010le,hwupload");
                a.insert(a.end(), {"-c:v", "hevc_vaapi", "-profile:v", "main10"});
                break;
            default:
                a.insert(a.end(), {"-c:v", "libx265", "-preset", "veryfast", "-crf", "23",
                                    "-pix_fmt", "yuv420p10le",
                                    "-x265-params", "hdr10=1:repeat-headers=1"});
        }
        a.insert(a.end(), {"-force_key_frames",
                            "expr:gte(t,n_forced*" + std::to_string(keyframeIntervalSecs) + ")"});
        // The source's *actual* color info, not bt709 — this is real HDR
        // output, unlike the forced-bt709 tagging in the SDR path below.
        a.insert(a.end(), {
            "-color_primaries", orDefaultColorTag(source_video->color_primaries, "bt2020"),
            "-color_trc",       orDefaultColorTag(source_video->color_transfer,  "smpte2084"),
            "-colorspace",      orDefaultColorTag(source_video->color_space,     "bt2020nc"),
        });
        return;
    }

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

    // Every branch above targets plain 8-bit yuv420p output regardless of
    // source, so the output is always structurally BT.709 SDR now — for an
    // HDR source, that's only true *pixel data* because of the tonemap
    // filter chain inserted above; without it, ffmpeg's automatic pix_fmt
    // conversion correctly truncates bit depth but leaves un-tone-mapped
    // PQ-range values in an 8-bit container while copying the *color tags*
    // through unchanged, producing output that's structurally SDR but whose
    // VUI metadata still claims BT.2020/SMPTE ST 2084 (PQ) HDR. Confirmed via
    // ffprobe against a real transcoded segment before the tonemap chain
    // existed: pix_fmt=yuv420p but color_transfer=smpte2084, color_primaries=
    // bt2020, color_space=bt2020nc. Browsers that respect embedded HDR
    // signaling (common on hardware-accelerated decode paths) can silently
    // fail to render that mismatch (no fatal error surfaces to hls.js/JS,
    // playback just never starts), and displays that do play it show
    // washed-out, hazy, crushed-contrast video (PQ-range values decoded
    // through a standard gamma EOTF). Retagging as bt709 unconditionally
    // here is what makes the declared color space match what the pixel data
    // actually is in both cases: genuinely SDR source untouched, HDR source
    // now genuinely tone-mapped to SDR above.
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
    // Plain dynaudnorm only targets peak level (p=0.95 by default) with
    // targetrms disabled -- most library content already peaks near full
    // scale, so peak-only normalization applies almost no gain and sounds
    // much quieter than loudnorm's old -16 LUFS target did. r= turns on
    // RMS-based targeting so quiet passages actually get boosted; m= raises
    // the max gain factor so there's enough headroom left to reach it.
    if (loudnorm) afParts.push_back("dynaudnorm=m=15:r=0.2");
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
