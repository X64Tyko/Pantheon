#pragma once
#include <cstdint>
#include <SQLiteCpp/SQLiteCpp.h>
#include <string>

class Database {
public:
    explicit Database(const std::string& path);

    SQLite::Database& get() { return db_; }

    // Open a second, independently-configured connection to the same file.
    // Callers that write from a background thread (e.g. SyncManager) should
    // hold their own connection so WAL-mode serialisation happens at the
    // SQLite level rather than requiring shared connection state.
    SQLite::Database openConnection() const;

private:
    void configure();
    void configure(SQLite::Database& db) const;
    void runMigrations();

    std::string      path_;
    SQLite::Database db_;
};
