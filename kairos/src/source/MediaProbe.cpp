#include "MediaProbe.h"
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

int64_t probeDurationMs(const std::string& file_path) {
    const std::string cmd =
        "timeout 10 ffprobe -v quiet -show_entries format=duration -of csv=p=0 "
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

std::vector<Chapter> probeChapters(const std::string& file_path) {
    const std::string cmd =
        "timeout 10 ffprobe -v quiet -print_format json -show_chapters "
        + shellQuote(file_path) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe))
        out += buf;
    pclose(pipe);
    if (out.empty()) return {};

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
