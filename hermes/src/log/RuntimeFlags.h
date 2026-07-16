#pragma once
#include <atomic>

// Mirrors Kairos's g_verbose_gateway_logs — kept fresh here by a background
// poller in main.cpp (see pollVerboseGatewayLogs) hitting Kairos's
// GET /api/config/public-settings, the same pattern Hephaestus uses for its
// own g_verbose_transcode_logs.
// Gates which of this process's own [hermes]/[roku-ecp] log lines reach the
// Hades Activity page via LogBuffer::push(); the log file always gets all of
// them regardless of this flag.
inline std::atomic<bool> g_verbose_gateway_logs{false};
