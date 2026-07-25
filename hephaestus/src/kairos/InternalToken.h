#pragma once
#include <string>

// Reads the `internal_token` key from kairos.conf's `[_global]` section (see
// kairos/src/conf/ConfStore.h) — read directly off the shared /data volume
// rather than over HTTP, since this must never be served by an
// unauthenticated route. Empty return means missing/unconfigured; callers
// should just omit the header and let Kairos's own check reject it.
std::string readKairosInternalToken(const std::string& conf_path);