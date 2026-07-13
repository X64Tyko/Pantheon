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
                       const std::string& library_type,
                       const std::string& preferred_scraper,
                       const std::string& preferred_language,
                       bool include_anidb,
                       bool skip_scraping);

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

    struct SourceBasicRow { std::string source_id, source_type, display_name; int syncPriority;};
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

    // ── User discovery ───────────────────────────────────────────────────────

    // Upserts discovered server accounts for a source (identity metadata
    // only). imported_user_id is preserved on conflict — this never un-imports
    // a row. Callers (SyncManager) are expected to have already excluded the
    // source's own configured primary account from `users`, so this table
    // only ever holds accounts Pantheon doesn't already treat as "the" sync
    // identity for that source.
    void upsertSourceUsers(const std::string& source_id,
                           const std::vector<SourceUserInfo>& users,
                           int64_t now);

    // imported_user_id is empty when this source user hasn't been imported
    // yet — the import route checks it to reject a re-import of an already-
    // imported account instead of silently creating a duplicate.
    struct SourceUserRow { std::string display_name, email, imported_user_id; };
    // Looked up by the import route to resolve display_name/email before
    // creating the local account.
    std::optional<SourceUserRow> getSourceUser(const std::string& source_id,
                                                const std::string& external_user_id);

    struct UnmappedSourceUserRow {
        std::string source_id;
        std::string source_display_name;
        std::string external_user_id;
        std::string display_name;
        std::string email;
    };
    // Every discovered account with no local Pantheon account imported for it yet.
    std::vector<UnmappedSourceUserRow> listUnmappedSourceUsers();

    // Marks a discovered source user as imported — it drops out of
    // listUnmappedSourceUsers() from this point on. Also used directly by the
    // "link to an existing user" route (SourceService's PUT .../link) — same
    // effect on this table either way, whether the local user is brand new
    // or already existed; see the v71 migration comment in Database.cpp.
    void setImportedUserId(const std::string& source_id, const std::string& external_user_id,
                           const std::string& user_id);

    struct SourceUserListRow {
        std::string external_user_id, display_name, email, imported_user_id; // imported_user_id empty = unlinked
    };
    // Every discovered account for one source, mapped or not — for the
    // per-source "link to an existing user" management panel (unlike
    // listUnmappedSourceUsers(), which only ever shows unlinked ones and
    // powers the separate global nag popup).
    std::vector<SourceUserListRow> listSourceUsers(const std::string& source_id);

    // Clears a previously-linked account back to unmapped. Does not touch
    // any watch_progress rows already written for the user it was linked to
    // — same no-cleanup precedent as unsetting media_source.synced_user_id.
    void unlinkSourceUser(const std::string& source_id, const std::string& external_user_id);

    // ── Watch-state sync target ──────────────────────────────────────────────

    // Which local user (if any) should have watch/resume state pulled from
    // this source's configured primary account during sync. Empty user_id clears it.
    void setSyncedUserId(const std::string& source_id, const std::string& user_id);
    // Returns "" if unset.
    std::string getSyncedUserId(const std::string& source_id);

private:
    Database& db_;
};
