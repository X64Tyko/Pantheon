#include "ConfigRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>

ConfigRepository::ConfigRepository(Database& db) : db_(db) {}

std::string ConfigRepository::getValue(const std::string& key) {
    SQLite::Statement q(db_.get(), "SELECT value FROM app_config WHERE key = ?");
    q.bind(1, key);
    return q.executeStep() ? q.getColumn(0).getString() : "";
}

void ConfigRepository::setValue(const std::string& key, const std::string& value) {
    SQLite::Statement s(db_.get(),
        "INSERT INTO app_config (key, value) VALUES (?,?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bind(1, key); s.bind(2, value); s.exec();
}
