#pragma once
#include <atomic>

// Mutable runtime flags — settable via PATCH /api/config/settings without restart.
// Initialized from env vars and persisted to app_config in the DB.
extern std::atomic<bool> g_epg_debug;
extern std::atomic<bool> g_debug_logging;
extern std::atomic<uint32_t> g_buffer_size;

inline bool epgDebug()     { return g_epg_debug.load(std::memory_order_relaxed); }
inline bool debugLogging() { return g_debug_logging.load(std::memory_order_relaxed); }
inline uint32_t bufferSize() { return g_buffer_size.load(std::memory_order_relaxed); }