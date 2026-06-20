#include "MediaProbe.h"
#include <cstdio>
#include <iostream>

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

int64_t probeDurationMs(const std::string& file_path) {
    const std::string cmd =
        "ffprobe -v quiet -show_entries format=duration -of csv=p=0 "
        + shellQuote(file_path) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return 0;
    char buf[64] = {};
    fgets(buf, sizeof(buf), pipe);
    pclose(pipe);
    try {
        const double secs = std::stod(buf);
        if (secs > 0.0)
            return static_cast<int64_t>(secs * 1000.0);
    } catch (const std::exception&) {}
    return 0;
}

} // namespace

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
