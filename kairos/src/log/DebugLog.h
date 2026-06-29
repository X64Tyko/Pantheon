#pragma once
#include <chrono>

// Enable with KAIROS_DEBUG=1 env var or --debug CLI arg.
extern bool g_debug_logging;

// Debug-only stream — usage: DLOG << "message\n";
// The including TU must also include <iostream>.
#define DLOG if (g_debug_logging) std::cout

// Returns elapsed milliseconds between two steady_clock time points.
inline long long elapsedMs(std::chrono::steady_clock::time_point t0,
                            std::chrono::steady_clock::time_point t1) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}
