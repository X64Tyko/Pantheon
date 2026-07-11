#include "DeviceRouter.h"
#include "RokuEcpClient.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

namespace {

// Hermes has no session store of its own (same reasoning as
// authedHephaestusProxy in api/Router.cpp) — every device route asks Kairos
// to validate the caller's bearer token and hands back user_id, which is
// what scopes ownership of both the Kairos-side roku_device row and the
// live DeviceSession.
std::optional<std::string> authenticate(const Config& cfg, const Req& req) {
    auto auth = req.get_header_value("Authorization");
    if (auth.empty()) return std::nullopt;
    httplib::Client cli(cfg.kairos_url);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);
    auto r = cli.Get("/api/auth/me", httplib::Headers{{"Authorization", auth}});
    if (!r || r->status != 200) return std::nullopt;
    try {
        auto j = json::parse(r->body);
        auto user_id = j.value("user_id", "");
        return user_id.empty() ? std::nullopt : std::optional(user_id);
    } catch (...) { return std::nullopt; }
}

// Same /api/auth/me round trip as authenticate() above, plus a role check —
// separate function (not a flag on authenticate) so every existing call site
// keeps its current "any logged-in user" semantics unchanged.
std::optional<std::string> authenticateAdmin(const Config& cfg, const Req& req) {
    auto auth = req.get_header_value("Authorization");
    if (auth.empty()) return std::nullopt;
    httplib::Client cli(cfg.kairos_url);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);
    auto r = cli.Get("/api/auth/me", httplib::Headers{{"Authorization", auth}});
    if (!r || r->status != 200) return std::nullopt;
    try {
        auto j = json::parse(r->body);
        auto user_id = j.value("user_id", "");
        if (user_id.empty() || j.value("role", "") != "admin") return std::nullopt;
        return user_id;
    } catch (...) { return std::nullopt; }
}

void unauthorized(Res& res) {
    res.status = 401;
    res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
}

// Matches Kairos's own convention of not distinguishing "not logged in" from
// "logged in but not admin" on its admin-only routes.
void forbidden(Res& res) {
    res.status = 403;
    res.set_content(json{{"error", "Forbidden"}}.dump(), "application/json");
}

void notConnected(Res& res) {
    res.status = 404;
    res.set_content(json{{"error", "Device not connected"}}.dump(), "application/json");
}

json sessionJson(const std::shared_ptr<DeviceSession>& s) {
    return {{"id", s->rokuDeviceId()}, {"state", s->state()}};
}

// Admin "who's connected" view — unlike sessionJson above (the cast-device-
// picker's own-devices scope, where the caller already knows it's their own
// account), this crosses user boundaries, so it surfaces user_id and
// liveness explicitly rather than just the raw state blob.
json adminSessionJson(const std::shared_ptr<DeviceSession>& s) {
    return {
        {"id",           s->rokuDeviceId()},
        {"user_id",      s->userId()},
        {"state",        s->state()},
        {"last_seen_ms", s->lastSeenMs()},
    };
}

} // namespace

void registerDeviceRoutes(httplib::Server& svr, DeviceSessionManager& devices, const Config& cfg) {

    // Called by the Roku channel on every boot once it knows its own
    // roku_device_id (from a prior pairing, persisted in roRegistry).
    svr.Post("/api/devices/register", [&devices, &cfg](const Req& req, Res& res) {
        auto user_id = authenticate(cfg, req);
        if (!user_id) { unauthorized(res); return; }
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400; res.set_content(json{{"error", "Invalid JSON"}}.dump(), "application/json"); return;
        }
        auto roku_device_id = body.value("roku_device_id", "");
        if (roku_device_id.empty()) {
            res.status = 400; res.set_content(json{{"error", "roku_device_id required"}}.dump(), "application/json"); return;
        }
        devices.getOrCreate(roku_device_id, *user_id);
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    // Called by the Roku channel the first time it's paired — after reading
    // pairingId/pairingToken off its ECP launch args (see
    // kairos/src/api/services/RokuDeviceService.cpp's POST /api/roku-devices).
    // Verifies with Kairos, then immediately brings the device online here
    // too, so the channel can start long-polling right away.
    svr.Post("/api/devices/confirm-pairing", [&devices, &cfg](const Req& req, Res& res) {
        auto auth = req.get_header_value("Authorization");
        auto user_id = authenticate(cfg, req);
        if (!user_id) { unauthorized(res); return; }
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400; res.set_content(json{{"error", "Invalid JSON"}}.dump(), "application/json"); return;
        }
        auto pairing_id    = body.value("pairing_id", "");
        auto pairing_token = body.value("pairing_token", "");
        if (pairing_id.empty() || pairing_token.empty()) {
            res.status = 400; res.set_content(json{{"error", "pairing_id and pairing_token required"}}.dump(), "application/json"); return;
        }

        httplib::Client kairos(cfg.kairos_url);
        kairos.set_connection_timeout(5);
        kairos.set_read_timeout(5);
        auto r = kairos.Post("/api/roku-devices/" + pairing_id + "/confirm-pairing",
                             httplib::Headers{{"Authorization", auth}},
                             json{{"pairing_token", pairing_token}}.dump(), "application/json");
        if (!r || r->status != 200) {
            res.status = r ? r->status : 502;
            res.set_content(r ? r->body : json{{"error", "Kairos unreachable"}}.dump(), "application/json");
            return;
        }

        devices.getOrCreate(pairing_id, *user_id);
        res.set_content(json{{"ok", true}, {"roku_device_id", pairing_id}}.dump(), "application/json");
    });

    // Caster's device picker (Hades) — only ever lists live sessions.
    // A known-but-currently-closed device (roku_device row with no live
    // session) doesn't appear here; Hades falls back to its Kairos-backed
    // /api/roku-devices list for "known devices" regardless of live state.
    svr.Get("/api/devices", [&devices, &cfg](const Req& req, Res& res) {
        auto user_id = authenticate(cfg, req);
        if (!user_id) { unauthorized(res); return; }
        json arr = json::array();
        for (auto& session : devices.listForUser(*user_id)) arr.push_back(sessionJson(session));
        res.set_content(arr.dump(), "application/json");
    });

    // Admin-only "who's connected" view — every live Roku session regardless
    // of owner, for deciding whether it's safe to restart/reboot while
    // people might be watching. Deliberately its own path rather than a
    // query param on GET /api/devices above, so that route's existing
    // "my own devices" contract (used unauthenticated-role-wise by every
    // regular user for their own cast-device picker) never has to branch on it.
    svr.Get("/api/devices/all", [&devices, &cfg](const Req& req, Res& res) {
        auto user_id = authenticateAdmin(cfg, req);
        if (!user_id) { forbidden(res); return; }
        json arr = json::array();
        for (auto& session : devices.listAll()) arr.push_back(adminSessionJson(session));
        res.set_content(arr.dump(), "application/json");
    });

    svr.Get(R"(/api/devices/([^/]+)$)", [&devices, &cfg](const Req& req, Res& res) {
        auto user_id = authenticate(cfg, req);
        if (!user_id) { unauthorized(res); return; }
        auto session = devices.get(req.matches[1]);
        if (!session || session->userId() != *user_id) { notConnected(res); return; }
        res.set_content(sessionJson(session).dump(), "application/json");
    });

    // The Roku's own long-poll — blocks up to 25s for a queued command, same
    // cv-wait-with-timeout shape as ChannelBroadcaster/LogBuffer::waitAfter
    // elsewhere in this codebase. 204 on timeout; the channel just calls
    // again immediately (classic long-poll, no persistent connection needed
    // across polls).
    svr.Get(R"(/api/devices/([^/]+)/commands/next$)", [&devices, &cfg](const Req& req, Res& res) {
        auto user_id = authenticate(cfg, req);
        if (!user_id) { unauthorized(res); return; }
        auto session = devices.get(req.matches[1]);
        if (!session || session->userId() != *user_id) { notConnected(res); return; }
        auto cmd = session->waitForCommand(25000);
        if (!cmd) { res.status = 204; return; }
        res.set_content(cmd->dump(), "application/json");
    });

    // The Roku posts its playback state here every couple of seconds while
    // something is loaded — casters read it back via GET /api/devices/:id.
    svr.Post(R"(/api/devices/([^/]+)/state$)", [&devices, &cfg](const Req& req, Res& res) {
        auto user_id = authenticate(cfg, req);
        if (!user_id) { unauthorized(res); return; }
        auto session = devices.get(req.matches[1]);
        if (!session || session->userId() != *user_id) { notConnected(res); return; }
        json body;
        try { body = json::parse(req.body); } catch (...) {
            res.status = 400; res.set_content(json{{"error", "Invalid JSON"}}.dump(), "application/json"); return;
        }
        session->setState(body);
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    // A caster (Hades) pushing "load"/"play"/"pause"/"seek"/"setVolume"/"stop".
    // Live session -> push straight onto its command queue (the Roku's
    // blocked long-poll above wakes immediately). Not live -> Kairos already
    // resolved this device's ip_address/app_id (same lookup also confirms
    // the caller owns it), so fall back to an ECP cold-launch, embedding the
    // command itself as launch params — the channel boots, logs in with its
    // stored token, and reads them directly from Main(args) without waiting
    // on a register+long-poll round trip for this first command.
    svr.Post(R"(/api/devices/([^/]+)/command$)", [&devices, &cfg](const Req& req, Res& res) {
        auto auth = req.get_header_value("Authorization");
        auto user_id = authenticate(cfg, req);
        if (!user_id) { unauthorized(res); return; }
        json command;
        try { command = json::parse(req.body); } catch (...) {
            res.status = 400; res.set_content(json{{"error", "Invalid JSON"}}.dump(), "application/json"); return;
        }
        const std::string roku_device_id = req.matches[1];

        auto session = devices.get(roku_device_id);
        if (session && session->userId() == *user_id) {
            session->pushCommand(command);
            res.set_content(json{{"ok", true}, {"status", "delivered"}}.dump(), "application/json");
            return;
        }

        httplib::Client kairos(cfg.kairos_url);
        kairos.set_connection_timeout(5);
        kairos.set_read_timeout(5);
        auto r = kairos.Get(("/api/roku-devices/" + roku_device_id).c_str(), httplib::Headers{{"Authorization", auth}});
        if (!r || r->status != 200) {
            res.status = r ? r->status : 502;
            res.set_content(r ? r->body : json{{"error", "Kairos unreachable"}}.dump(), "application/json");
            return;
        }
        json device;
        try { device = json::parse(r->body); } catch (...) {
            res.status = 502; res.set_content(json{{"error", "Malformed Kairos response"}}.dump(), "application/json"); return;
        }

        std::map<std::string, std::string> params;
        for (auto& [k, v] : command.items()) {
            if (v.is_string()) params[k] = v.get<std::string>();
            else if (!v.is_null()) params[k] = v.dump();
        }

        if (!RokuEcpClient::launch(device.value("ip_address", ""), device.value("app_id", "dev"), params)) {
            res.status = 502; res.set_content(json{{"error", "Roku unreachable"}}.dump(), "application/json"); return;
        }
        res.set_content(json{{"ok", true}, {"status", "launching"}}.dump(), "application/json");
    });
}
