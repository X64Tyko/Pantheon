#include "RokuDeviceService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../../db/RokuDeviceRepository.h"
#include "../../db/DbHelpers.h"
#include "../../roku/RokuEcpClient.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <ctime>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

namespace {

constexpr int64_t kPairingTtlSecs = 5 * 60;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Short-lived, single-use — delivered directly to the device's own IP via
// ECP launch params, never stored anywhere else. mt19937_64-via-random_device
// (db::generateId's source) is adequate for that threat model; concatenated
// twice for 128 bits.
std::string generatePairingToken() {
    return db::generateId() + db::generateId();
}

json deviceJson(const RokuDevice& d) {
    return {
        {"id",         d.id},
        {"name",       d.name},
        {"ip_address", d.ip_address},
        {"app_id",     d.app_id},
        {"paired",     d.paired()},
        {"created_at", d.created_at},
    };
}

} // namespace

RokuDeviceService::RokuDeviceService(const ServiceContext& ctx) : db_(ctx.db) {}

void RokuDeviceService::registerRoutes(httplib::Server& svr) {

    svr.Get("/api/roku-devices", [this](const Req&, Res& res) {
        if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
        json arr = json::array();
        for (const auto& d : RokuDeviceRepository(db_).listForUser(currentUser()->user_id))
            arr.push_back(deviceJson(d));
        route::ok(res, arr.dump());
    });

    // Creates the row, resolves the real ECP app_id (falls back to "dev" —
    // the ECP launch target for a sideloaded/dev-mode channel — if the
    // device is unreachable or nothing named "Pantheon" is installed), then
    // immediately ECP-launches it with pairing params. Works whether the
    // channel is already running or not (supports_input_launch=1 in the
    // manifest), so this single call covers both cases.
    svr.Post("/api/roku-devices", [this](const Req& req, Res& res) {
        if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
        json body;
        try { body = json::parse(req.body); } catch (...) { route::err(res, 400, "Invalid JSON"); return; }
        const std::string name       = body.value("name", "");
        const std::string ip_address = body.value("ip_address", "");
        if (name.empty() || ip_address.empty()) { route::err(res, 400, "name and ip_address required"); return; }

        std::string app_id = "dev";
        for (const auto& app : RokuEcpClient::queryApps(ip_address)) {
            if (lower(app.name).find("pantheon") != std::string::npos) { app_id = app.id; break; }
        }

        const std::string pairing_token = generatePairingToken();
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const std::string id = RokuDeviceRepository(db_).create(
            currentUser()->user_id, name, ip_address, app_id, pairing_token, now + kPairingTtlSecs);

        RokuEcpClient::launch(ip_address, app_id, {
            {"pairingId",    id},
            {"pairingToken", pairing_token},
        });

        route::ok(res, json{{"id", id}, {"pairing_status", "waiting"}}.dump());
    });

    svr.Get("/api/roku-devices/:id", [this](const Req& req, Res& res) {
        if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
        auto d = RokuDeviceRepository(db_).get(req.path_params.at("id"));
        if (!d || d->user_id != currentUser()->user_id) { route::err(res, 404, "Not found"); return; }
        route::ok(res, deviceJson(*d).dump());
    });

    svr.Delete("/api/roku-devices/:id", [this](const Req& req, Res& res) {
        if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
        if (!RokuDeviceRepository(db_).remove(req.path_params.at("id"), currentUser()->user_id)) {
            route::err(res, 404, "Not found"); return;
        }
        route::ok(res, json{{"ok", true}}.dump());
    });

    // Called by Hermes (hermes/src/devices/DeviceRouter.cpp), forwarding the
    // confirming Roku's own bearer token, once the freshly (re)launched
    // channel reads its pairingId/pairingToken launch args and asks to
    // confirm — not called by the Roku channel directly, since it has no
    // way to reach Kairos except through Hermes. The pairing_token itself is
    // the real security boundary (single-use, 128-bit, 5 minute TTL); the
    // user_id match below is defense in depth against a confused deputy.
    svr.Post("/api/roku-devices/:id/confirm-pairing", [this](const Req& req, Res& res) {
        if (!currentUser()) { route::err(res, 401, "Unauthorized"); return; }
        json body;
        try { body = json::parse(req.body); } catch (...) { route::err(res, 400, "Invalid JSON"); return; }
        const std::string pairing_token = body.value("pairing_token", "");

        auto d = RokuDeviceRepository(db_).get(req.path_params.at("id"));
        if (!d || d->user_id != currentUser()->user_id) { route::err(res, 404, "Not found"); return; }

        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (!RokuDeviceRepository(db_).confirmPairing(req.path_params.at("id"), pairing_token, now)) {
            route::err(res, 400, "Invalid or expired pairing token"); return;
        }
        route::ok(res, json{{"ok", true}}.dump());
    });
}
