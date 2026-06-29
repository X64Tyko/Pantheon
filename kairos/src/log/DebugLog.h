#pragma once
#include <chrono>
#include "scheduler/RuntimeFlags.h"

// Debug-only stream — usage: DLOG << "message\n";
// Enabled via KAIROS_DEBUG=1 env var, --debug CLI arg, or the Settings UI.
#define DLOG if (g_debug_logging.load(std::memory_order_relaxed)) std::cout

// Returns elapsed milliseconds between two steady_clock time points.
inline long long elapsedMs(std::chrono::steady_clock::time_point t0,
                            std::chrono::steady_clock::time_point t1) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}
