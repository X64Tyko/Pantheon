#pragma once
#include <atomic>
#include <string>

// Mutable runtime flags — settable via PATCH /api/config/settings without restart.
// Initialized from env vars and persisted to app_config in the DB.
extern std::atomic<bool> g_epg_debug;
extern std::atomic<bool> g_debug_logging;
extern std::atomic<uint32_t> g_buffer_size;
// Read by Hephaestus via GET /api/config/settings, not consumed by Kairos
// itself — controls whether spawned ffmpeg processes log verbosely.
extern std::atomic<bool> g_verbose_transcode_logs;
// Read by Hephaestus via GET /api/config/public-settings, not consumed by
// Kairos itself — controls whether each spawned ffmpeg's stderr is streamed
// live into Hephaestus's own log (and from there, /api/logs/stream) instead
// of only being tail-captured for an on-failure dump. Independent of
// verbose_transcode_logs above, which only controls ffmpeg's own log level/
// diagnostic filters, not whether anything forwards it. Was HEPH_FFMPEG_DEBUG,
// an env var fixed at Hephaestus startup, until this — same runtime-settable
// treatment as verbose_transcode_logs so live diagnosis doesn't need a restart.
extern std::atomic<bool> g_ffmpeg_debug_logs;
// Read by Hermes via GET /api/config/public-settings, not consumed by Kairos
// itself — controls whether Hermes's own [hermes] access-log lines and
// [roku-ecp] device-unreachable warnings reach the Hades Activity page.
extern std::atomic<bool> g_verbose_gateway_logs;
// Read by Hades itself via GET /api/config/settings — gates whether Hades's
// console.error override (see hades/src/remoteLog.ts) forwards to
// POST /api/logs/client at all. Uncaught errors/rejections forward
// regardless of this flag; console.error is the noisy, opt-in case.
extern std::atomic<bool> g_hades_debug;

inline bool epgDebug() { return g_epg_debug.load(std::memory_order_relaxed); }
inline bool debugLogging() { return g_debug_logging.load(std::memory_order_relaxed); }
inline uint32_t bufferSize() { return g_buffer_size.load(std::memory_order_relaxed); }
inline bool verboseTranscodeLogs() { return g_verbose_transcode_logs.load(std::memory_order_relaxed); }
inline bool ffmpegDebugLogs() { return g_ffmpeg_debug_logs.load(std::memory_order_relaxed); }
inline bool verboseGatewayLogs() { return g_verbose_gateway_logs.load(std::memory_order_relaxed); }
inline bool hadesDebug() { return g_hades_debug.load(std::memory_order_relaxed); }

// LogBuffer::setFilter() target for log_buffer in main.cpp. Named (rather
// than an inline lambda at the call site) for the same reason hermes/
// hephaestus's equivalents are — a single source of truth a unit test could
// exercise directly if kairos ever grows a LogBuffer test like theirs.
inline bool kairosLogFilter(const std::string& line)
{
	if (line.starts_with("[scraper]") || line.starts_with("[sync]")) return true;
	if (line.starts_with("[epg]") || line.starts_with("[materializer]")) return g_epg_debug.load(std::memory_order_relaxed);
	if (line.starts_with("[sync-advanced]") || line.starts_with("[probe]") || line.starts_with("[scraper-match]")) return g_debug_logging.load(std::memory_order_relaxed);
	if (line.starts_with("[hls]") || line.starts_with("[session]")) return g_verbose_transcode_logs.load(std::memory_order_relaxed);
	return true;
}