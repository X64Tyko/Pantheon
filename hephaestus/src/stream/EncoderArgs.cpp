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
                                "-rc:v", "vbr", "-cq", "23", "-pix_fmt", "yuv420p"});
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
}

void pushAudioEncoderArgs(std::vector<std::string>& a, bool loudnorm, double speed,
                           int audio_bitrate_kbps) {
    std::vector<std::string> afParts;
    if (loudnorm) afParts.push_back("loudnorm=I=-16:TP=-1.5:LRA=11");
    if (speed != 1.0) afParts.push_back("atempo=" + fmtSpeed(speed));

    a.insert(a.end(), {"-c:a", "aac", "-b:a", std::to_string(audio_bitrate_kbps) + "k"});
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
