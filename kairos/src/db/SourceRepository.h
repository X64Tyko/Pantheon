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
                              const std::string& library_type,
                              const std::string& preferred_scraper = "",
                              const std::string& preferred_language = "",
                              bool include_anidb = false);

    void updateLibrary(const std::string& library_id,
                       const std::string& display_name,
                       const std::string& preferred_scraper,
                       const std::string& preferred_language,
                       bool include_anidb);

    void removeLibrary(const std::string& library_id);
    std::optional<MediaLibraryConfig> getLibrary(const std::string& library_id);

    // Focused setter for the Home-shelf visibility flag — separate from
    // updateLibrary so the per-card "hide from Home" shortcut (which only
    // knows a library_id, not the rest of the edit form's fields) has a
    // minimal call to make.
    void setLibraryShowOnHome(const std::string& library_id, bool show_on_home);

    // Ordered scraper preference for this library, split by item_type so a
    // mixed library can rank differently for shows vs movies. Empty = no
    // preference (matching falls back to score alone, same as an empty
    // preferred_scraper did before this existed).
    std::vector<std::string> getScraperPriority(const std::string& library_id,
                                                 const std::string& item_type);
    void setScraperPriority(const std::string& library_id, const std::string& item_type,
                            const std::vector<std::string>& order);

    // ── Source mapping ────────────────────────────────────────────────────────

    // Resolve a source-native external_id to a Kairos internal ID.
    std::string resolveKairosId(const std::string& source_id,
                                const std::string& external_id,
                                const std::string& item_type);

    // Sample a raw file path for a given source (for path-mapping UI).
    std::optional<std::string> samplePath(const std::string& source_id);

    struct SourceMappingRow { std::string source_id, external_id; };
    std::optional<SourceMappingRow> getSourceMapping(const std::string& kairos_id);

    struct SourceBasicRow { std::string source_id, source_type, display_name; };
    std::vector<SourceBasicRow> listSourcesBasic();

    // Returns external_lib_id for a library, or empty string if not found.
    std::string getExternalLibId(const std::string& library_id, const std::string& source_id);

    struct ResolvedSource {
        std::string source_id;
        std::string external_id;
        std::string file_path;
    };
    // Resolves kairos_id → source_id + external_id + file_path via source_mapping JOIN.
    std::optional<ResolvedSource> resolveItemSource(const std::string& item_type,
                                                     const std::string& kairos_id);

    // Returns base_url for a source, or empty string if not found.
    std::string getSourceBaseUrl(const std::string& source_id);

    // Returns full source config for a single source.
    std::optional<MediaSourceConfig> getSource(const std::string& source_id);

    // Returns external_id for an item in a specific source, or empty string if not found.
    std::string getExternalId(const std::string& source_id,
                               const std::string& kairos_id,
                               const std::string& item_type);

    struct WritebackTarget {
        std::string source_id;
        std::string source_type;    // "plex" | "jellyfin" | "emby" | "local"
        std::string base_url;
        std::string external_id;
        std::string external_lib_id; // Plex "section" id; empty if not tracked for this mapping
        std::string display_name;   // media_source.display_name — for the detail panel's "Sources" list
    };
    // Every source this show/movie is mapped to, with what's needed to push
    // metadata back to each one. Usually one row, but not assumed — an item
    // can be mapped to more than one source_mapping row (different sources).
    std::vector<WritebackTarget> getWritebackTargets(const std::string& item_type,
                                                      const std::string& kairos_id);

private:
    Database& db_;
};
