#pragma once
#include <string>
#include <unordered_map>

class Database;

// Field-level metadata overrides (see architecture.md "Metadata Override Layer").
// Lets a single field (e.g. overview) survive future source syncs without
// locking the whole show/movie row the way the coarse `locked` column does.
class MetadataOverrideRepository {
public:
    explicit MetadataOverrideRepository(Database& db);

    // field_name -> value for every override on this entity.
    std::unordered_map<std::string, std::string> getAll(const std::string& entity_type,
                                                          const std::string& entity_id);

    // Upsert one field. set_by "user" is never auto-cleared; "scraper_accept" is
    // meant to be cleared once a later sync's value matches it again (not yet
    // produced by any write path — see architecture.md's sync cleanup pass).
    void set(const std::string& entity_type, const std::string& entity_id,
             const std::string& field_name, const std::string& value,
             const std::string& set_by = "user");

    // Explicit unlock — remove one field's override so future syncs resume updating it.
    void remove(const std::string& entity_type, const std::string& entity_id,
                const std::string& field_name);

private:
    Database& db_;
};
