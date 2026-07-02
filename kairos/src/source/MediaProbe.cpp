#include "MediaProbe.h"
#include "log/DebugLog.h"
#include <cstdio>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace {

constexpr int64_t kMinMs =        1'000; // 1 second
constexpr int64_t kMaxMs = 86'400'000;   // 24 hours

// Wrap s in single quotes, escaping any interior single quotes.
std::string shellQuote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else           r += c;
    }
    return r + "'";
}

// Primary: container-level duration from the format header (fast).
int64_t probeFormatDurationMs(const std::string& file_path) {
    const std::string cmd =
        "timeout -k 2 10 ffprobe -v quiet -show_entries format=duration -of csv=p=0 "
        + shellQuote(file_path) + " 2>/dev/null";
    DLOG << "[probe] format-duration cmd: " << cmd << '\n';
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return 0;
    char buf[64] = {};
    fgets(buf, sizeof(buf), pipe);
    pclose(pipe);
    try {
        const double secs = std::stod(buf);
        if (secs > 0.0) return static_cast<int64_t>(secs * 1000.0);
    } catch (...) {}
    return 0;
}

// Fallback: per-track stream duration.  Matroska track headers are stored near
// the start of the file even when the segment SegmentInfo Duration element is
// absent (common in some Blu-ray MKV encodes).  Returns the longest track
// duration so the result reflects whichever track runs longest.
int64_t probeStreamDurationMs(const std::string& file_path) {
    const std::string cmd =
        "timeout -k 2 10 ffprobe -v quiet -show_entries stream=duration -of csv=p=0 "
        + shellQuote(file_path) + " 2>/dev/null";
    DLOG << "[probe] stream-duration cmd: " << cmd << '\n';
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return 0;
    int64_t best = 0;
    char buf[64] = {};
    while (fgets(buf, sizeof(buf), pipe)) {
        try {
            const double secs = std::stod(buf);
            const int64_t ms = static_cast<int64_t>(secs * 1000.0);
            if (ms > best) best = ms;
        } catch (...) {}
    }
    pclose(pipe);
    return best;
}

int64_t probeDurationMs(const std::string& file_path) {
    const auto t0 = std::chrono::steady_clock::now();

    int64_t result = probeFormatDurationMs(file_path);
    if (result > 0) {
        DLOG << "[probe] format-duration " << elapsedMs(t0, std::chrono::steady_clock::now())
             << "ms → " << result << "ms: " << file_path << '\n';
        return result;
    }

    result = probeStreamDurationMs(file_path);
    DLOG << "[probe] stream-duration fallback " << elapsedMs(t0, std::chrono::steady_clock::now())
         << "ms → " << result << "ms: " << file_path << '\n';
    return result;
}

} // namespace

std::vector<Chapter> probeChapters(const std::string& file_path) {
    const std::string cmd =
        "timeout -k 2 10 ffprobe -v quiet -print_format json -show_chapters "
        + shellQuote(file_path) + " 2>/dev/null";
    DLOG << "[probe] chapters cmd: " << cmd << '\n';
    const auto t0 = std::chrono::steady_clock::now();
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        DLOG << "[probe] chapters popen failed: " << file_path << '\n';
        return {};
    }
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe))
        out += buf;
    pclose(pipe);
    const long long ms = elapsedMs(t0, std::chrono::steady_clock::now());
    if (out.empty()) {
        DLOG << "[probe] chapters done in " << ms << "ms → no output: " << file_path << '\n';
        return {};
    }

    std::vector<Chapter> result;
    try {
        auto j = json::parse(out);
        if (!j.contains("chapters")) return result;
        int pos = 0;
        for (const auto& ch : j["chapters"]) {
            Chapter c;
            c.chapter_type = "unclassified";
            c.source       = "file";
            c.position     = pos++;
            if (ch.contains("tags") && ch["tags"].is_object()
                    && ch["tags"].contains("title"))
                c.title = ch["tags"]["title"].get<std::string>();
            if (ch.contains("start_time") && ch["start_time"].is_string()) {
                try {
                    c.start_ms = static_cast<int64_t>(
                        std::stod(ch["start_time"].get<std::string>()) * 1000.0);
                } catch (...) {}
            }
            if (ch.contains("end_time") && ch["end_time"].is_string()) {
                try {
                    c.end_ms = static_cast<int64_t>(
                        std::stod(ch["end_time"].get<std::string>()) * 1000.0);
                } catch (...) {}
            }
            result.push_back(std::move(c));
        }
    } catch (const std::exception& e) {
        std::cerr << "[probe] chapter parse error for " << file_path
                  << ": " << e.what() << '\n';
    }
    DLOG << "[probe] chapters done in " << ms << "ms → " << result.size()
         << " chapter(s): " << file_path << '\n';
    return result;
}

StreamLanguages probeStreamLanguages(const std::string& file_path) {
    const std::string cmd =
        "timeout -k 2 15 ffprobe -v quiet -print_format json -show_streams "
        + shellQuote(file_path) + " 2>/dev/null";
    DLOG << "[probe] streams cmd: " << cmd << '\n';
    const auto t0 = std::chrono::steady_clock::now();
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        DLOG << "[probe] streams popen failed: " << file_path << '\n';
        return {};
    }
    std::string out;
    char buf[8192];
    while (fgets(buf, sizeof(buf), pipe))
        out += buf;
    pclose(pipe);
    const long long ms = elapsedMs(t0, std::chrono::steady_clock::now());
    if (out.empty()) {
        DLOG << "[probe] streams done in " << ms << "ms → no output: " << file_path << '\n';
        return {};
    }

    StreamLanguages result;
    try {
        auto j = json::parse(out);
        if (!j.contains("streams")) return result;
        for (const auto& s : j["streams"]) {
            const std::string type = s.value("codec_type", "");
            std::string lang;
            if (s.contains("tags") && s["tags"].is_object()) {
                auto& t = s["tags"];
                if      (t.contains("language"))  lang = t["language"].get<std::string>();
                else if (t.contains("LANGUAGE"))  lang = t["LANGUAGE"].get<std::string>();
            }
            if (lang.empty() || lang == "und") continue;
            if      (type == "audio")    result.audio.push_back(lang);
            else if (type == "subtitle") result.subtitle.push_back(lang);
        }
    } catch (const std::exception& e) {
        std::cerr << "[probe] stream-lang parse error for " << file_path
                  << ": " << e.what() << '\n';
    }
    DLOG << "[probe] streams done in " << ms << "ms → "
         << result.audio.size() << " audio, "
         << result.subtitle.size() << " subtitle track(s): " << file_path << '\n';
    return result;
}

namespace {
// ffprobe reports "bits_per_raw_sample" as a JSON string (e.g. "10"), and
// not every container/muxer populates it reliably — pix_fmt's "10le"/"12le"
// suffix (e.g. "yuv420p10le") is a solid fallback when it's absent. Same
// approach as Hephaestus's own MediaProbe (src/stream/MediaProbe.cpp) —
// kept as a separate copy since these are two different services/binaries,
// not shared code.
int parseBitDepth(const json& s) {
    if (s.contains("bits_per_raw_sample")) {
        try {
            auto raw = s["bits_per_raw_sample"].get<std::string>();
            if (!raw.empty()) return std::stoi(raw);
        } catch (...) {}
    }
    std::string pix_fmt = s.value("pix_fmt", "");
    if (pix_fmt.find("10le") != std::string::npos || pix_fmt.find("10be") != std::string::npos) return 10;
    if (pix_fmt.find("12le") != std::string::npos || pix_fmt.find("12be") != std::string::npos) return 12;
    return 8;
}
} // namespace

VideoInfo probeVideoInfo(const std::string& file_path) {
    const std::string cmd =
        "timeout -k 2 15 ffprobe -v quiet -print_format json -show_streams "
        + shellQuote(file_path) + " 2>/dev/null";
    DLOG << "[probe] videoinfo cmd: " << cmd << '\n';
    const auto t0 = std::chrono::steady_clock::now();
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        DLOG << "[probe] videoinfo popen failed: " << file_path << '\n';
        return {};
    }
    std::string out;
    char buf[8192];
    while (fgets(buf, sizeof(buf), pipe))
        out += buf;
    pclose(pipe);
    const long long ms = elapsedMs(t0, std::chrono::steady_clock::now());
    if (out.empty()) {
        DLOG << "[probe] videoinfo done in " << ms << "ms → no output: " << file_path << '\n';
        return {};
    }

    VideoInfo result;
    try {
        auto j = json::parse(out);
        if (!j.contains("streams")) return result;
        for (const auto& s : j["streams"]) {
            if (s.value("codec_type", "") != "video") continue;
            result.codec     = s.value("codec_name", "");
            result.width     = s.value("width", 0);
            result.height    = s.value("height", 0);
            result.bit_depth = parseBitDepth(s);
            break; // first video stream only
        }
    } catch (const std::exception& e) {
        std::cerr << "[probe] videoinfo parse error for " << file_path
                  << ": " << e.what() << '\n';
    }
    DLOG << "[probe] videoinfo done in " << ms << "ms → " << result.codec << " "
         << result.width << "x" << result.height << " " << result.bit_depth
         << "-bit: " << file_path << '\n';
    return result;
}

int64_t validateDurationMs(int64_t dur, const std::string& file_path) {
    if (dur >= kMinMs && dur <= kMaxMs)
        return dur;

    if (dur != 0) {
        std::cerr << "[probe] out-of-range duration_ms=" << dur
                  << " for " << file_path << " — probing with ffprobe\n";
    }

    const int64_t probed = probeDurationMs(file_path);
    if (probed >= kMinMs && probed <= kMaxMs) {
        std::cout << "[probe] " << file_path
                  << " — ffprobe duration: " << probed << " ms\n";
        return probed;
    }

    std::cerr << "[probe] WARNING: could not determine valid duration for "
              << file_path << " (source=" << dur
              << " ffprobe=" << probed << ")\n";
    return 0;
}
