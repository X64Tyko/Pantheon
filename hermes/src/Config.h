#pragma once
#include <string>
#include <cstdlib>

struct Config {
    std::string hephaestus_url = "http://localhost:8082";
    std::string kairos_url     = "http://localhost:8080";
    int         port           = 8000;
    int         linger_secs    = 30;

    // HDHomeRun identity — Hermes owns this, Hephaestus should be internal-only
    std::string hdhr_device_id   = "50414e54"; // "PANT" in ASCII hex
    std::string hdhr_friendly    = "Pantheon";
    int         hdhr_tuner_count = 4;
};

inline Config parseConfig(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string k = argv[i];
        std::string v = argv[i + 1];
        if      (k == "--hephaestus-url") { cfg.hephaestus_url = v;           ++i; }
        else if (k == "--kairos-url")     { cfg.kairos_url = v;               ++i; }
        else if (k == "--port")           { cfg.port = std::stoi(v);          ++i; }
        else if (k == "--linger")         { cfg.linger_secs = std::stoi(v);   ++i; }
        else if (k == "--device-id")      { cfg.hdhr_device_id = v;           ++i; }
        else if (k == "--friendly-name")  { cfg.hdhr_friendly = v;            ++i; }
        else if (k == "--tuners")         { cfg.hdhr_tuner_count = std::stoi(v); ++i; }
    }
    if (auto* p = getenv("HEPHAESTUS_URL")) cfg.hephaestus_url = p;
    if (auto* p = getenv("KAIROS_URL"))     cfg.kairos_url     = p;
    return cfg;
}
