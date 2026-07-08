#include "RokuDeviceRepository.h"
#include "Database.h"
#include "DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>

namespace {

RokuDevice rowToDevice(SQLite::Statement& q) {
    RokuDevice d;
    d.id                 = q.getColumn(0).getString();
    d.user_id            = q.getColumn(1).getString();
    d.name               = q.getColumn(2).getString();
    d.ip_address         = q.getColumn(3).getString();
    d.app_id             = q.getColumn(4).getString();
    if (!q.getColumn(5).isNull()) d.pairing_token = q.getColumn(5).getString();
    d.pairing_expires_at = q.getColumn(6).getInt64();
    d.created_at         = q.getColumn(7).getInt64();
    return d;
}

constexpr const char* kSelectCols =
    "id, user_id, name, ip_address, app_id, pairing_token, pairing_expires_at, created_at";

} // namespace

RokuDeviceRepository::RokuDeviceRepository(Database& db) : db_(db) {}

std::vector<RokuDevice> RokuDeviceRepository::listForUser(const std::string& user_id) {
    SQLite::Statement q(db_.get(),
        std::string("SELECT ") + kSelectCols + " FROM roku_device WHERE user_id = ? ORDER BY created_at DESC");
    q.bind(1, user_id);
    std::vector<RokuDevice> out;
    while (q.executeStep()) out.push_back(rowToDevice(q));
    return out;
}

std::optional<RokuDevice> RokuDeviceRepository::get(const std::string& id) {
    SQLite::Statement q(db_.get(),
        std::string("SELECT ") + kSelectCols + " FROM roku_device WHERE id = ?");
    q.bind(1, id);
    if (!q.executeStep()) return std::nullopt;
    return rowToDevice(q);
}

std::string RokuDeviceRepository::create(const std::string& user_id, const std::string& name,
                                         const std::string& ip_address, const std::string& app_id,
                                         const std::string& pairing_token, int64_t pairing_expires_at) {
    const std::string id = db::generateId();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));

    SQLite::Statement s(db_.get(), R"(
        INSERT INTO roku_device (id, user_id, name, ip_address, app_id, pairing_token, pairing_expires_at, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )");
    s.bind(1, id);
    s.bind(2, user_id);
    s.bind(3, name);
    s.bind(4, ip_address);
    s.bind(5, app_id);
    s.bind(6, pairing_token);
    s.bind(7, pairing_expires_at);
    s.bind(8, now);
    s.exec();
    return id;
}

bool RokuDeviceRepository::confirmPairing(const std::string& id, const std::string& pairing_token, int64_t now) {
    SQLite::Statement q(db_.get(),
        "SELECT pairing_token, pairing_expires_at FROM roku_device WHERE id = ?");
    q.bind(1, id);
    if (!q.executeStep()) return false;
    if (q.getColumn(0).isNull()) return false; // already paired
    if (q.getColumn(0).getString() != pairing_token) return false;
    if (q.getColumn(1).getInt64() < now) return false; // expired

    SQLite::Statement u(db_.get(),
        "UPDATE roku_device SET pairing_token = NULL, pairing_expires_at = 0 WHERE id = ?");
    u.bind(1, id);
    u.exec();
    return true;
}

bool RokuDeviceRepository::remove(const std::string& id, const std::string& user_id) {
    SQLite::Statement d(db_.get(), "DELETE FROM roku_device WHERE id = ? AND user_id = ?");
    d.bind(1, id);
    d.bind(2, user_id);
    d.exec();
    return db_.get().getChanges() > 0;
}
