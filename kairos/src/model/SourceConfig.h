#pragma once
#include <cstdint>
#include <optional>
#include <string>

struct MediaSourceConfig {
    std::string source_id;
    std::string source_type; // "plex" | "jellyfin" | "emby" | "local"
    std::string display_name;
    std::string base_url;    // empty for local sources
    bool        enabled = true;
    // Which local user (if any) should have watch/resume state pulled from
    // this source's configured primary account during sync. Empty = unset —
    // see SourceRepository::setSyncedUserId / SyncManager::syncMovies.
    std::string synced_user_id;
    // See IMediaSource::lastUserDiscoveryError(). Empty = last user-discovery
    // sync succeeded (or none has run yet); otherwise the last failure,
    // refreshed every sync.
    std::string user_sync_error;
    // Lower syncs/wins first — drives both SyncManager::loadSources()' sync
    // order and, via show/movie.primary_source, which source's data wins a
    // field-level conflict when the same item is matched across sources.
    // Default 0 for every source until the user actually ranks them, so
    // behavior is unchanged (ties, first-writer-keeps) until they opt in.
    int sync_priority = 0;
};

struct MediaLibraryConfig {
    std::string library_id;
    std::string source_id;
    std::string external_lib_id;
    std::string display_name;
    std::string library_type;         // "show" | "movie" | "mixed" | "music" | "photo"
    std::string preferred_scraper;    // "" | "tmdb" | "tvdb" | "anidb"
    std::string preferred_language;   // ISO 639-1 e.g. "en", "ja"; empty = scraper default
    bool        enabled = true;
    // AniDB is anime-only — never queried automatically for a library unless
    // explicitly opted in here (or preferred_scraper is set to "anidb" outright).
    bool        include_anidb = false;
    // Off for filler/bumper/commercial libraries — excluded from the unscoped
    // (no library_id) show/movie queries backing Home's shelves, but still
    // fully usable for channel building and direct Library browsing.
    bool        show_on_home = true;
    // On for filler/bumper/home-video libraries — this library's items never
    // enter the scraper match queue at all (see ScraperManager::runMatch()).
    bool        skip_scraping = false;
};

// Returned by MediaSource::listAvailableLibraries() — live from the server
struct LibraryInfo {
    std::string external_lib_id;
    std::string name;
    std::string type; // "show" | "movie" | "mixed" | "music" | "photo"
};

// An account/profile that exists on the source server — identity metadata
// only (never credentials). Returned by MediaSource::listServerUsers() — live
// from the server. Used to detect source-side users Pantheon doesn't know
// about yet.
struct SourceUserInfo {
    std::string external_user_id;
    std::string display_name;
    std::string email; // empty if the source doesn't expose one for this account
};

// One item's watch/resume state for a specific *non-primary* source account
// — returned by MediaSource::fetchWatchState(external_user_id). Distinct
// from Movie/Episode's own src_watched/src_position_ms/src_watched_at (which
// only ever reflect the source's single configured primary identity); this
// is how any other discovered SourceUserInfo's progress gets pulled in.
struct ExternalWatchState {
    std::string external_id;   // source-native item id (matches source_mapping.external_id)
    std::string item_type;     // "movie" | "episode"
	std::string title;
    std::optional<bool>    watched;
    std::optional<int64_t> position_ms{0};
    std::optional<int64_t> watched_at{0}; // epoch seconds, if the source provides one
    std::optional<int64_t> view_count{0}; // real rewatch count when the source reports one
};
