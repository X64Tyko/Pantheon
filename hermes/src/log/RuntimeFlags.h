#pragma once
#include <atomic>
#include <string>

// Mirrors Kairos's g_verbose_gateway_logs — kept fresh here by a background
// poller in main.cpp (see pollVerboseGatewayLogs) hitting Kairos's
// GET /api/config/public-settings, the same pattern Hephaestus uses for its
// own g_verbose_transcode_logs.
// Gates which of this process's own [hermes]/[roku-ecp] log lines reach the
// Hades Activity page via LogBuffer::push(); the log file always gets all of
// them regardless of this flag.
inline std::atomic<bool> g_verbose_gateway_logs{false};

// LogBuffer::setFilter() target for local_log in main.cpp. Named (rather
// than an inline lambda at the call site) so momus/hermes/test_log_buffer.cpp
// can exercise the exact same policy instead of re-deriving it and silently
// drifting out of sync.
inline bool hermesLogFilter(const std::string& line) {
    if (line.starts_with("[hermes]") || line.starts_with("[roku-ecp]"))
        return g_verbose_gateway_logs.load(std::memory_order_relaxed);
    return true;
}
