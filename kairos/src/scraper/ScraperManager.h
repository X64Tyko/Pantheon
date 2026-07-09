#pragma once
#include <atomic>
#include <memory>
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
    bool acceptCandidate(const std::string& candidate_id);
    bool rejectCandidate(const std::string& candidate_id);

    // Manually pin a specific external result as the match for an item.
    // Stores a candidate at score 1.0 then accepts it.
    bool manualMatch(const std::string& kairos_id,
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

    // Returns the CDN poster URL for an AniDB AID, or empty if unavailable/disabled.
    std::string anidbPosterUrl(const std::string& aid) const;

    // See AnidbScraper::rateLimitImageWait(). No-op if AniDB isn't configured.
    void anidbRateLimitImage() const;

private:
    void buildScrapers();
    void runMatch(const std::string& target_id, const std::string& item_type);

    void matchShow (const std::string& library_id, const std::string& kairos_id, const std::string& title,
                    int year, const std::string& tmdb_id, const std::string& tvdb_id,
                    bool include_anidb = false);
    void matchMovie(const std::string& library_id, const std::string& kairos_id, const std::string& title,
                    int year, const std::string& tmdb_id, const std::string& file_path,
                    bool include_anidb = false);

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

    Database&    db_;
    ConfStore&   conf_;

    class SourceRepository& sourceRepo() const;

    std::unique_ptr<AnidbScraper> anidb_;
    std::unique_ptr<TmdbScraper> tmdb_;
    std::unique_ptr<TvdbScraper> tvdb_;
    std::atomic<bool>            matching_{false};
};
