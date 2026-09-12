#pragma once
#include <atomic>
#include <string>

// Mirrors Kairos's g_verbose_transcode_logs — kept fresh here by
// SessionManager::refreshCache()'s existing ~15s/5min poll of Kairos's
// GET /api/config/public-settings (see cached_verbose_transcode_logs).
// Gates which of this process's own [ffmpeg]/[sessions] log lines reach the
// Hades Activity page via LogBuffer::push(); the log file always gets all of
// them regardless of this flag.
inline std::atomic<bool> g_verbose_transcode_logs{false};

// LogBuffer::setFilter() target for log_buffer in main.cpp. Named (rather
// than an inline lambda at the call site) so
// momus/hephaestus/test_log_buffer.cpp can exercise the exact same policy
// instead of re-deriving it and silently drifting out of sync.
inline bool hephaestusLogFilter(const std::string& line) {
    if (line.starts_with("[ffmpeg]") || line.starts_with("[sessions]"))
        return g_verbose_transcode_logs.load(std::memory_order_relaxed);
    return true;
}
