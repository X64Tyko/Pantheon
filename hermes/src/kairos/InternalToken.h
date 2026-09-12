#pragma once
#include <string>

// Reads the `internal_token` key from kairos.conf's `[_global]` section (see
// kairos/src/conf/ConfStore.h) — read directly off the shared /data volume
// rather than over HTTP, since this must never be served by an
// unauthenticated route. Empty return means missing/unconfigured; callers
// should just omit the header and let the internal-only route reject it.
// Mirrors hephaestus/src/kairos/InternalToken.h exactly — Hermes needs the
// same value for the same reason (relayUpstreamLogs authenticating its pull
// of Kairos's and Hephaestus's own /api/logs/stream), and it's small enough
// that duplicating it locally (matching how each service already keeps its
// own KairosClient rather than sharing one) is simpler than introducing a
// shared/ dependency for one function.
std::string readKairosInternalToken(const std::string& conf_path);