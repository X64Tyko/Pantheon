#pragma once
#include <optional>
#include <string>
#include <vector>

class Database;

// A user's paired Roku channel — id/name/ip_address are what Hades and
// Hermes need; app_id/pairing_token/pairing_expires_at are internal to the
// pairing + ECP-launch flow (see RokuEcpClient, RokuDeviceService).
struct RokuDevice {
    std::string id;
    std::string user_id;
    std::string name;
    std::string ip_address;
    std::string app_id;
    std::optional<std::string> pairing_token;
    int64_t     pairing_expires_at = 0;
    int64_t     created_at         = 0;

    bool paired() const { return !pairing_token.has_value(); }
};

class RokuDeviceRepository {
public:
    explicit RokuDeviceRepository(Database& db);

    std::vector<RokuDevice> listForUser(const std::string& user_id);
    std::optional<RokuDevice> get(const std::string& id);

    // Creates the row with a fresh pairing_token (unpaired) — returns the new id.
    std::string create(const std::string& user_id, const std::string& name,
                       const std::string& ip_address, const std::string& app_id,
                       const std::string& pairing_token, int64_t pairing_expires_at);

    // Returns false if id not found, already paired, token mismatched, or expired.
    bool confirmPairing(const std::string& id, const std::string& pairing_token, int64_t now);

    // Scoped to the owning user — returns false if no such device for this user_id.
    bool remove(const std::string& id, const std::string& user_id);

private:
    Database& db_;
};
