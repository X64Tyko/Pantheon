#pragma once
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

class Database;
class ConfStore;
class AnidbScraper;
class TmdbScraper;
class TvdbScraper;

struct ScraperCandidate {
    std::string candidate_id;
    std::string item_type;     // "show" | "movie"
    std::string kairos_id;
    std::string source;        // "tmdb" | "tvdb"
    std::string external_id;
    std::string title;
    int         year        = 0;
    double      score       = 0.0;
    int         accepted    = -1;  // -1=pending, 1=accepted, 0=rejected
    std::string poster_url;
    std::string overview;
};

struct QueueItem {
    std::string kairos_id;
    std::string item_type;
    std::string title;
    int         year        = 0;
    std::string thumb;
    std::string source_id;
    std::string source_base_url;
    std::string match_status;
    double      match_score = 0.0;
    std::vector<ScraperCandidate> candidates;
    // On-disk folder for this item — movie.file_path's parent dir, or (for
    // shows) the common ancestor across all episode file_paths. Lets a
    // reviewer tell which physical folder a queue entry corresponds to.
    std::string folder_path;
};

struct ScraperStats {
    int total     = 0;
    int matched   = 0;
    int uncertain = 0;
    int unmatched = 0;
    int unscraped = 0;
    // skip_scraping items (item-level or via their library) — never queued,
    // counted separately so they don't read as permanently-stuck "unscraped".
    int skipped   = 0;
};

struct SpecialCandidate {
    std::string candidate_id, show_id;
    int         episode_number = 0;
    std::string special_title, special_overview, special_air_date, special_thumb;
    std::string source, source_episode_id;
    std::string movie_id, movie_title;
    int         movie_year = 0;
    double      score = 0.0;
    int         accepted = -1; // -1=pending, 1=accepted, 0=rejected
};

struct LinkedSpecial {
    std::string episode_id;
    int         episode_number = 0;
    std::string title;
    std::string linked_movie_id;
    std::string linked_movie_title;
};

// Result of acceptCandidate()/manualMatch(). merged_into_kairos_id is
// non-empty iff the accepted item turned out to duplicate an external id
// already owned by a different item — in that case `kairos_id` was merged
// into merged_into_kairos_id and deleted, and callers must not touch it
// further (e.g. re-fetch its detail).
struct AcceptResult {
    bool        found = false;              // false = candidate/item not found
    std::string item_type;                  // "show" | "movie"
    std::string merged_into_kairos_id;
    std::string merged_into_title;
    bool        folder_mismatch = false;    // merge auto-forced past a folder_path mismatch
};

struct ScraperConfig {
    std::string source;
    std::string api_key;
    std::string language;
    std::string pin;      // TVDB subscriber pin (optional)
    bool        enabled         = false;
    double      language_weight = 0.1;
};

struct ScraperSettings {
    std::vector<ScraperConfig> configs;
    double match_threshold = 0.8;
    // Sync-time cross-source dedup thresholds (SyncManager) — not proven
    // constants like match_threshold, proposed defaults expecting real-world
    // tuning after rollout. A fuzzy (non-exact) title match at or above
    // dedup_fuzzy_title_threshold, or an exact folder match paired with title
    // similarity below dedup_folder_corroboration_threshold, is flagged as an
    // uncertain duplicate candidate for human review rather than auto-merged.
    double dedup_fuzzy_title_threshold          = 0.80;
    double dedup_folder_corroboration_threshold = 0.30;
};

class ScraperManager {
public:
    ScraperManager(Database& db, ConfStore& conf);
    ~ScraperManager();

    // Kick off a background match pass.  target_id + item_type optionally scope
    // the pass to a single item; empty strings → match all unscraped items.
    void triggerMatch(const std::string& target_id   = "",
                      const std::string& item_type   = "");

    // Synchronous variant — runs inline on the calling thread.
    // Used by SyncManager so matching completes before chapter detection begins.
    void runMatchSync(const std::string& target_id   = "",
                      const std::string& item_type   = "");

    bool isMatching() const { return matching_.load(); }

    // Re-pulls full metadata (title, overview, posters, ratings, ...) from
    // each already-linked source for every matched show/movie — same
    // per-item logic as refreshMetadata(), just swept across the whole
    // library in the background. For catching drift after a manual match
    // fix, or after applyShowMetadata/applyMovieMetadata gain new fields.
    void triggerRefreshAll();
    bool isRefreshingAll() const { return refreshing_all_.load(); }

    // Settings
    ScraperSettings getSettings() const;
    void            updateSettings(const ScraperSettings& s);

    // Multi-ID and alternate titles
    struct ExternalId {
        std::string source;
        std::string external_id;
        int         priority = 0;
    };
    std::vector<ExternalId> getExternalIds(const std::string& kairos_id, const std::string& item_type) const;
    void                    setExternalIds(const std::string& kairos_id, const std::string& item_type, const std::vector<ExternalId>& ids);

    std::vector<std::string> getAlternateTitles(const std::string& kairos_id, const std::string& item_type) const;
    void                     setAlternateTitles(const std::string& kairos_id, const std::string& item_type, const std::vector<std::string>& titles);

    // Review queue
    std::vector<QueueItem> getQueue(const std::string& status_filter, // "uncertain"|"unmatched"|"all"
                                    int limit, int offset) const;
    int queueTotal(const std::string& status_filter) const;

    // Accept / reject a single candidate
    AcceptResult acceptCandidate(const std::string& candidate_id);
    bool rejectCandidate(const std::string& candidate_id);

    // Manually pin a specific external result as the match for an item.
    // Stores a candidate at score 1.0 then accepts it.
    AcceptResult manualMatch(const std::string& kairos_id,
                     const std::string& item_type,
                     const std::string& source,
                     const std::string& external_id,
                     const std::string& title,
                     int year,
                     const std::string& poster_url,
                     const std::string& overview);

    // Re-fetches and re-applies full metadata (overview, genres, images, etc.)
    // from whichever scraper is already matched to this item — same
    // fetch-and-apply path as accepting a candidate, just re-run against the
    // existing match instead of a newly-searched one. Locked fields are still
    // respected. False if the item has never had a confirmed match to refresh.
    bool refreshMetadata(const std::string& kairos_id, const std::string& item_type);

    // Live search of enabled scrapers
    struct SearchResult {
        std::string source;
        std::string external_id;
        std::string title;
        int         year = 0;
        std::string overview;
        std::string poster_url;
        std::string content_type;  // "show" | "movie"
        bool        in_library = false;
        std::string library_id;      // this library's show_id/movie_id when in_library; empty otherwise
        std::string request_status;  // most relevant existing content_request status ("pending"|"approved"|"rejected"), by ANY user; empty = never requested
    };
    std::vector<SearchResult> search(const std::string& query,
                                     const std::string& content_type) const;

    ScraperStats stats() const;

    // Show specials <-> movie-library-file linking. Scans every scraper the
    // show has a confirmed ID with for season-0 (specials/OVA/TV-movie)
    // entries, merges duplicates found across sources, and scores them
    // against the movie catalog. Never auto-links — always goes through
    // accept/reject, same as the main review queue.
    std::vector<SpecialCandidate> scanSpecialsForShow(const std::string& show_id);
    std::vector<SpecialCandidate> getSpecialCandidates(const std::string& show_id) const;
    std::vector<LinkedSpecial>    getLinkedSpecials(const std::string& show_id) const;
    bool acceptSpecialCandidate(const std::string& candidate_id);
    bool rejectSpecialCandidate(const std::string& candidate_id);

    // Returns the CDN poster URL for an AniDB AID, or empty if unavailable/disabled.
    std::string anidbPosterUrl(const std::string& aid) const;

    // See AnidbScraper::rateLimitImageWait(). No-op if AniDB isn't configured.
    void anidbRateLimitImage() const;

private:
    void buildScrapers();
    void runMatch(const std::string& target_id, const std::string& item_type);
    void runRefreshAll();

    // An item can be mapped to more than one media_library (cross-source
    // dedup — e.g. the same show synced from both a Plex library and a Local
    // library that disagree on preferred_language/scraper priority). There is
    // deliberately no single "winning" library: settings are merged instead —
    // scraper priority lists concatenated de-duplicated (earlier-mapped
    // library's order wins ties), include_anidb is true if ANY mapped library
    // wants it, and preferred_language becomes a set (a scraper matching ANY
    // member gets the language bonus) rather than one value silently
    // overwriting the other. See resolveMatchSettings().
    struct MatchSettings {
        std::set<std::string>    preferred_languages;
        std::vector<std::string> priority;
        bool                     include_anidb = false;
    };
    MatchSettings resolveMatchSettings(const std::string& item_type, const std::string& kairos_id) const;

    void matchShow (const MatchSettings& settings, const std::string& kairos_id, const std::string& title,
                    int year, const std::string& tmdb_id, const std::string& tvdb_id);
    void matchMovie(const MatchSettings& settings, const std::string& kairos_id, const std::string& title,
                    int year, const std::string& tmdb_id, const std::string& file_path);

    void  storeCandidate(const std::string& item_type, const std::string& kairos_id,
                         const std::string& source,    const std::string& external_id,
                         const std::string& title,     int year, double score,
                         const std::string& poster_url = "",
                         const std::string& overview   = "");
    void  setMatchStatus(const std::string& item_type, const std::string& kairos_id,
                         const std::string& status,   double score);
    void  upsertExternalId(const std::string& item_type, const std::string& kairos_id,
                           const std::string& source,    const std::string& external_id,
                           int priority);
    void  linkExternalId(const std::string& item_type, const std::string& kairos_id,
                         const std::string& source,    const std::string& external_id,
                         bool promote_to_primary);
    void  upsertAlternateTitle(const std::string& item_type, const std::string& kairos_id,
                               const std::string& title);
    double threshold() const;

    // Returns the kairos_id of a DIFFERENT show/movie already linked to
    // (source, external_id) in item_external_id, or "" if none. Excludes
    // exclude_kairos_id. Inner-joins against the live show/movie table so a
    // stale/orphaned item_external_id row can never be reported as an owner.
    std::string findExternalIdOwner(const std::string& item_type,
                                     const std::string& source,
                                     const std::string& external_id,
                                     const std::string& exclude_kairos_id) const;

    struct DuplicateMergeResult {
        std::string merged_into_kairos_id;
        std::string merged_into_title;
        bool        folder_mismatch = false;
    };

    // If (item_type, source, external_id) is already owned by a different
    // kairos_id, merges `kairos_id` into that owner (deleting `kairos_id`)
    // and returns the outcome. Otherwise returns a default-constructed
    // (empty) result. Callers must stop all further processing of
    // `kairos_id` when merged_into_kairos_id is non-empty.
    DuplicateMergeResult mergeIfDuplicateExternalId(const std::string& item_type,
                                                     const std::string& kairos_id,
                                                     const std::string& source,
                                                     const std::string& external_id);

    // Every (source, external_id) this show is confirmed to be identified by,
    // merging all three places that fact can live: item_external_id (richest —
    // populated once a human confirms via acceptCandidate, and since the auto-
    // match paths now also call linkExternalId(), most auto-matches too), the
    // winning item_match_candidate row (belt-and-suspenders for any auto-match
    // predating that), and the show's own tvdb_id/tmdb_id columns (covers rows
    // synced before either code path ran). De-duplicated by source, richest-first.
    std::vector<std::pair<std::string, std::string>>
        confirmedShowSourceIds(const std::string& show_id) const;

    Database&    db_;
    ConfStore&   conf_;

    class SourceRepository& sourceRepo() const;

    std::unique_ptr<AnidbScraper> anidb_;
    std::unique_ptr<TmdbScraper> tmdb_;
    std::unique_ptr<TvdbScraper> tvdb_;
    std::atomic<bool>            matching_{false};
    std::atomic<bool>            refreshing_all_{false};
};
