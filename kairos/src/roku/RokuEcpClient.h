#pragma once
#include <map>
#include <string>
#include <vector>

// Talks Roku's External Control Protocol (port 8060, no auth — LAN-only by
// design on Roku's side) directly to a device's IP. Used only for: (1)
// resolving the real app_id at pairing time via queryApps, and (2) the
// cold-launch fallback when Hermes has no live device-session for a known
// Roku (see hermes/src/devices/DeviceRouter.cpp). Never used for ongoing
// playback control — that's the device-session channel's job.
class RokuEcpClient {
public:
    struct App {
        std::string id;
        std::string name;
    };

    // GET /query/apps — lists installed channels. Empty on any failure
    // (unreachable device, timeout); callers should fall back to "dev".
    static std::vector<App> queryApps(const std::string& ip_address);

    // GET /launch/:app_id?k=v&... — deep-launches (or, if already running
    // and the channel's manifest sets supports_input_launch=1, delivers an
    // input event to) the given app. Returns false only on a network-level
    // failure; a non-2xx HTTP status from the Roku is not distinguishable
    // from success without a device on hand to test against, so this is
    // intentionally best-effort.
    static bool launch(const std::string& ip_address, const std::string& app_id,
                       const std::map<std::string, std::string>& params);
};
