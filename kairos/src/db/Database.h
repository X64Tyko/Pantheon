#pragma once
#include <cstdint>
#include <SQLiteCpp/SQLiteCpp.h>
#include <string>

class Database {
public:
    explicit Database(const std::string& path);

    SQLite::Database& get() { return db_; }

private:
    void configure();
    void runMigrations();

    SQLite::Database db_;
};
