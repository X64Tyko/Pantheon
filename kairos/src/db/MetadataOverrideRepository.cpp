#include "MetadataOverrideRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>

MetadataOverrideRepository::MetadataOverrideRepository(Database& db) : db_(db) {}

std::unordered_map<std::string, std::string> MetadataOverrideRepository::getAll(
    const std::string& entity_type, const std::string& entity_id) {
    SQLite::Statement q(db_.get(),
        "SELECT field_name, value FROM metadata_override WHERE entity_type=? AND entity_id=?");
    q.bind(1, entity_type);
    q.bind(2, entity_id);

    std::unordered_map<std::string, std::string> out;
    while (q.executeStep())
        out[q.getColumn(0).getString()] = q.getColumn(1).getString();
    return out;
}

void MetadataOverrideRepository::set(const std::string& entity_type, const std::string& entity_id,
                                      const std::string& field_name, const std::string& value,
                                      const std::string& set_by) {
    SQLite::Statement s(db_.get(), R"(
        INSERT INTO metadata_override (entity_type, entity_id, field_name, value, set_by)
        VALUES (?, ?, ?, ?, ?)
        ON CONFLICT(entity_type, entity_id, field_name)
        DO UPDATE SET value = excluded.value, set_by = excluded.set_by,
                      created_at = strftime('%Y-%m-%dT%H:%M:%SZ','now')
    )");
    s.bind(1, entity_type);
    s.bind(2, entity_id);
    s.bind(3, field_name);
    s.bind(4, value);
    s.bind(5, set_by);
    s.exec();
}

void MetadataOverrideRepository::remove(const std::string& entity_type, const std::string& entity_id,
                                         const std::string& field_name) {
    SQLite::Statement s(db_.get(),
        "DELETE FROM metadata_override WHERE entity_type=? AND entity_id=? AND field_name=?");
    s.bind(1, entity_type);
    s.bind(2, entity_id);
    s.bind(3, field_name);
    s.exec();
}
