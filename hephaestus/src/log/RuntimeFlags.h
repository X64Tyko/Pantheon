#pragma once
#include <atomic>

// Mirrors Kairos's g_verbose_transcode_logs — kept fresh here by
// SessionManager::refreshCache()'s existing ~15s/5min poll of Kairos's
// GET /api/config/public-settings (see cached_verbose_transcode_logs).
// Gates which of this process's own [ffmpeg]/[sessions] log lines reach the
// Hades Activity page via LogBuffer::push(); the log file always gets all of
// them regardless of this flag.
inline std::atomic<bool> g_verbose_transcode_logs{false};
