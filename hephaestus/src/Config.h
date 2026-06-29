#pragma once
#include <string>
#include <cstdlib>

struct Config {
    std::string kairos_url    = "http://localhost:8080";
    std::string ffmpeg_path   = "ffmpeg";
    std::string ffprobe_path  = "ffprobe";
    int         port          = 8082;
    std::string audio_lang    = "eng";
    bool        loudnorm      = false;
    int         session_linger_secs = 60; // keep session alive after last client disconnects

    // HDHomeRun device identity presented to Plex / Emby / Jellyfin
    std::string hdhr_device_id   = "48455048"; // "HEPH" in ASCII hex
    std::string hdhr_friendly    = "Hephaestus";
    int         hdhr_tuner_count = 4;
};

inline Config parseConfig(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string k = argv[i];
        std::string v = argv[i + 1];
        if      (k == "--kairos-url")    { cfg.kairos_url = v;              ++i; }
        else if (k == "--ffmpeg")        { cfg.ffmpeg_path = v;             ++i; }
        else if (k == "--ffprobe")       { cfg.ffprobe_path = v;            ++i; }
        else if (k == "--port")          { cfg.port = std::stoi(v);         ++i; }
        else if (k == "--audio-lang")    { cfg.audio_lang = v;              ++i; }
        else if (k == "--loudnorm")      { cfg.loudnorm = (v != "0" && v != "false"); ++i; }
        else if (k == "--linger")        { cfg.session_linger_secs = std::stoi(v); ++i; }
        else if (k == "--device-id")     { cfg.hdhr_device_id = v;          ++i; }
        else if (k == "--friendly-name") { cfg.hdhr_friendly = v;           ++i; }
        else if (k == "--tuners")        { cfg.hdhr_tuner_count = std::stoi(v); ++i; }
    }
    if (auto* p = getenv("KAIROS_URL"))    cfg.kairos_url   = p;
    if (auto* p = getenv("FFMPEG_PATH"))   cfg.ffmpeg_path  = p;
    if (auto* p = getenv("FFPROBE_PATH"))  cfg.ffprobe_path = p;
    if (auto* p = getenv("HEPH_LOUDNORM")) cfg.loudnorm     = (std::string(p) != "0");
    return cfg;
}
