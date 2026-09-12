#pragma once
#include "KairosTypes.h"
#include <optional>
#include <string>
#include <vector>

class KairosClient {
    std::string base_url_;
public:
    explicit KairosClient(std::string base_url);
    std::vector<KairosChannel> getChannels();
    // nullopt = unreachable/malformed response — caller keeps its last-known
    // value rather than treating that as "flag is off" (see hephaestus's
    // identical getVerboseTranscodeLogs() for the same reasoning).
    std::optional<bool> getVerboseGatewayLogs();
};
