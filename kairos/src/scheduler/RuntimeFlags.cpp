#include "RuntimeFlags.h"
#include <cstdlib>
#include <string>

static bool initEpgDebug() {
    const char* e = std::getenv("KAIROS_DEBUG_EPG");
    return e && std::string(e) == "1";
}

std::atomic<bool> g_epg_debug{initEpgDebug()};
