#pragma once
#include <httplib.h>

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
//
// Registered under /stream/ (not /api/) so it rides the same Hermes proxy
// pattern already used for /stream/vod/*, /stream/preview/* etc — Hermes's
// generic /api/.* proxy goes to Kairos, and Hephaestus's own /api/logs/stream
// is already shadowed by Hermes's own log endpoint at that same path, so
// /api/ was not an option here.
void registerActivityRoutes(httplib::Server& svr, SessionManager& sessions,
                             VodSessionManager& vodSessions, LogBuffer& logs);
