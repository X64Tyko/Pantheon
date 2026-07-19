#pragma once
#include <httplib.h>
#include <string>

class ClientCapabilityCache;

// "Bearer <token>" -> "<token>"; empty if the header is missing/malformed.
// Shared with the Hephaestus Router's own /stream/vod/start handler (which
// needs the same token to look up a client's declared capability before
// deciding direct-play), rather than duplicating this in both places.
std::string extractBearerToken(const httplib::Request& req);

// Client-declared decode capability, keyed by bearer token — lets
// isDirectPlayable() (VodSession.cpp) make a per-client direct-play
// decision instead of a fixed global allowlist. See ClientCapabilities.h
// for the full design (in-memory, restart-wiped, why that's fine).
//
//   POST   /stream/client-capabilities  -- declare {"video_codecs":[...],"audio_codecs":[...]}
//   DELETE /stream/client-capabilities  -- forget this token's declaration (call on logout)
//
// Registered under /stream/ (not /api/) for the same reason
// ActivityRouter.h's routes are — rides Hermes's existing authed
// Hephaestus-proxy pattern (the token itself is what identifies the
// caller here, no separate auth check needed beyond "a token was
// present"), and Hephaestus's own /api/ namespace is already spoken for by
// unrelated routes.
void registerClientCapabilitiesRoutes(httplib::Server& svr, ClientCapabilityCache& cache);
