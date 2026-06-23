#pragma once
#include <atomic>

// Mutable runtime flags — settable via PATCH /api/config/settings without restart.
// Initialized from env vars (KAIROS_DEBUG_EPG, etc.) on first use.
extern std::atomic<bool> g_epg_debug;

inline bool epgDebug() { return g_epg_debug.load(std::memory_order_relaxed); }
