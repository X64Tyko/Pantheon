#include "EncoderArgs.h"
#include <sstream>
#include <iomanip>

std::string fmtSpeed(double speed) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << speed;
    return ss.str();
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
