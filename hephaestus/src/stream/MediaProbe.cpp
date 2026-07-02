#include "MediaProbe.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

using json = nlohmann::json;

static std::string runCommand(const std::string& cmd) {
    std::array<char, 4096> buf{};
    std::string result;
    auto closer = [](FILE* f) { if (f) pclose(f); };
    std::unique_ptr<FILE, decltype(closer)> pipe(popen(cmd.c_str(), "r"), closer);
    if (!pipe) return "";
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe.get()))
        result += buf.data();
    return result;
}

std::optional<MediaInfo> probeMedia(const std::string& ffprobe_path,
                                     const std::string& file_path) {
    // Shell-escape the file path by wrapping in single quotes (works for paths
    // without embedded single quotes, which covers all sane media file names).
    std::string safe_path;
    for (char c : file_path) {
        if (c == '\'') safe_path += "'\\''";
        else safe_path += c;
    }

    std::string cmd = ffprobe_path
        + " -v quiet -print_format json -show_streams '"
        + safe_path + "' 2>/dev/null";

    std::string output = runCommand(cmd);
    if (output.empty()) return std::nullopt;

    try {
        auto j = json::parse(output);
        MediaInfo info;
        int audio_rel = 0, sub_rel = 0;

        for (auto& s : j.value("streams", json::array())) {
            std::string codec_type = s.value("codec_type", "");
            std::string lang       = "";
            std::string title      = "";
            if (s.contains("tags")) {
                auto& tags = s["tags"];
                lang  = tags.value("language", tags.value("LANGUAGE", ""));
                title = tags.value("title",    tags.value("TITLE",    ""));
            }

            if (codec_type == "video") {
                VideoTrack v;
                v.stream_index = s.value("index", 0);
                v.codec        = s.value("codec_name", "");
                v.width        = s.value("width",  0);
                v.height       = s.value("height", 0);
                info.video.push_back(v);
            } else if (codec_type == "audio") {
                AudioTrack a;
                a.stream_index  = s.value("index", 0);
                a.relative_index = audio_rel++;
                a.codec         = s.value("codec_name", "");
                a.language      = lang;
                a.title         = title;
                a.channels      = s.value("channels", 0);
                info.audio.push_back(a);
            } else if (codec_type == "subtitle") {
                SubtitleTrack st;
                st.stream_index  = s.value("index", 0);
                st.relative_index = sub_rel++;
                st.codec         = s.value("codec_name", "");
                st.language      = lang;
                st.title         = title;
                info.subtitles.push_back(st);
            }
        }
        return info;
    } catch (const std::exception& e) {
        std::cerr << "[probe] JSON parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

std::optional<MediaInfo> probeMediaCached(const std::string& ffprobe_path,
                                           const std::string& file_path) {
    static std::mutex cache_mtx;
    static std::unordered_map<std::string, MediaInfo> cache;
    {
        std::lock_guard<std::mutex> lock(cache_mtx);
        auto it = cache.find(file_path);
        if (it != cache.end()) return it->second;
    }
    auto info = probeMedia(ffprobe_path, file_path);
    if (!info) return std::nullopt;
    std::lock_guard<std::mutex> lock(cache_mtx);
    cache.emplace(file_path, *info);
    return info;
}

int pickAudioTrack(const MediaInfo& info, const std::string& preferred_lang) {
    if (info.audio.empty()) return 0;
    if (!preferred_lang.empty()) {
        for (auto& a : info.audio) {
            // Compare first 3 chars for ISO 639-2 vs 639-1 tolerance
            if (a.language.substr(0, 3) == preferred_lang.substr(0, 3))
                return a.relative_index;
        }
    }
    return 0;
}

int pickSubtitleTrack(const MediaInfo& info, const std::string& preferred_lang) {
    if (preferred_lang.empty() || info.subtitles.empty()) return -1;
    for (auto& s : info.subtitles) {
        if (s.language.substr(0, 3) == preferred_lang.substr(0, 3))
            return s.relative_index;
    }
    return -1;
}
