#pragma once
#include "../stream/ChannelSession.h" // HwAccel
#include <httplib.h>
#include <string>

class SessionManager;
class VodSessionManager;
class LogBuffer;

// Session-activity endpoints (currently-playing listing + per-session log
// tail), for Hades' Activity page "Now Playing" panel. Kept in its own
// router file rather than folded into api/Router.cpp/registerRoutes() so
// this addition doesn't collide with unrelated work on the main
// stream-serving routes.
//
//   GET /stream/activity/sessions            -- active channel/VOD sessions
//   GET /stream/activity/sessions/:id/logs   -- recent log lines tagged with
//                                                that session/channel id
//                                                (?lines=N, default 500)
//   GET /stream/activity/metrics             -- process CPU/RAM, plus a
//                                                "gpu" object (see
//                                                GpuMetrics.h) when
//                                                gpu_backend != HwAccel::none
//
// Registered under /stream/ (not /api/) so it rides the same Hermes proxy
// pattern already used for /stream/vod/*, /stream/preview/* etc — Hermes's
// generic /api/.* proxy goes to Kairos, and Hephaestus's own /api/logs/stream
// is already shadowed by Hermes's own log endpoint at that same path, so
// /api/ was not an option here.
//
// gpu_backend/vaapi_device are HwCapabilities' *resolved* backend (main.cpp
// picks encode if non-none, else decode) and Config::vaapi_device — passed
// once at startup, then queried fresh on every /metrics request the same
// way process CPU/RAM already is, not cached.
void registerActivityRoutes(httplib::Server& svr, SessionManager& sessions,
                             VodSessionManager& vodSessions, LogBuffer& logs,
                             HwAccel gpu_backend, const std::string& vaapi_device);
