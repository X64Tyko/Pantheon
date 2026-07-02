#include "RuntimeFlags.h"
#include <cstdlib>
#include <string>

static bool envTrue(const char* var) {
    const char* v = std::getenv(var);
    return v && std::string(v) == "1";
}

std::atomic<bool> g_epg_debug{envTrue("KAIROS_DEBUG_EPG")};
std::atomic<bool> g_debug_logging{envTrue("KAIROS_DEBUG")};
std::atomic<uint32_t>  g_buffer_size{1024}; // KB — mirrors ConfStore's 1048576-byte default
std::atomic<bool> g_verbose_transcode_logs{false}; // Hephaestus-only flag, no Kairos env override
