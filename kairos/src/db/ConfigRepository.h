#pragma once
#include <string>

class Database;

class ConfigRepository {
public:
    explicit ConfigRepository(Database& db);

    std::string getValue(const std::string& key);
    void        setValue(const std::string& key, const std::string& value);

private:
    Database& db_;
};
