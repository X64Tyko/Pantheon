#pragma once
#include <optional>
#include <string>
#include <vector>
#include "../model/SourceConfig.h"

class Database;

class SourceRepository {
public:
    explicit SourceRepository(Database& db);

    // ── Media sources ─────────────────────────────────────────────────────────

    std::vector<MediaSourceConfig> listSources();

    void createSource(const std::string& source_id,
                      const std::string& source_type,
                      const std::string& display_name,
                      const std::string& base_url);

    void removeSource(const std::string& source_id);

    // ── Libraries ─────────────────────────────────────────────────────────────

    std::vector<MediaLibraryConfig> listLibraries(const std::string& source_id);

    std::string createLibrary(const std::string& source_id,
                              const std::string& external_lib_id,
                              const std::string& display_name,
                              const std::string& library_type);

    void removeLibrary(const std::string& library_id);

    // ── Source mapping ────────────────────────────────────────────────────────

    // Resolve a source-native external_id to a Kairos internal ID.
    std::string resolveKairosId(const std::string& source_id,
                                const std::string& external_id,
                                const std::string& item_type);

    // Sample a raw file path for a given source (for path-mapping UI).
    std::optional<std::string> samplePath(const std::string& source_id);

private:
    Database& db_;
};
