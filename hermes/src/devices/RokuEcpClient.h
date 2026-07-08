#pragma once
#include <map>
#include <string>

// Launch-only counterpart of kairos/src/roku/RokuEcpClient — duplicated
// rather than shared across build targets (same pattern as LogBuffer.h
// existing as an identical copy in hephaestus/hermes/kairos). Kairos owns
// app_id resolution (queryApps, at "Add Roku" time); Hermes only ever needs
// the launch call itself, as the cold-launch fallback when no live
// DeviceSession is registered for a command's target (see DeviceRouter.cpp).
class RokuEcpClient {
public:
    static bool launch(const std::string& ip_address, const std::string& app_id,
                       const std::map<std::string, std::string>& params);
};
