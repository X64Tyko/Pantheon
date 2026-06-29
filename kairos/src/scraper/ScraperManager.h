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
    bool        enabled   = false;
};

struct ScraperSettings {
    std::vector<ScraperConfig> configs;
    double match_threshold = 1.0;
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
    };
    std::vector<SearchResult> search(const std::string& query,
                                     const std::string& content_type) const;

    ScraperStats stats() const;

    // Returns the CDN poster URL for an AniDB AID, or empty if unavailable/disabled.
    std::string anidbPosterUrl(const std::string& aid) const;

private:
    void buildScrapers();
    void runMatch(const std::string& target_id, const std::string& item_type);

    void matchShow (const std::string& kairos_id, const std::string& title,
                    int year, const std::string& tmdb_id, const std::string& tvdb_id,
                    const std::string& preferred_scraper = "");
    void matchMovie(const std::string& kairos_id, const std::string& title,
                    int year, const std::string& tmdb_id, const std::string& file_path,
                    const std::string& preferred_scraper = "");

    void  storeCandidate(const std::string& item_type, const std::string& kairos_id,
                         const std::string& source,    const std::string& external_id,
                         const std::string& title,     int year, double score,
                         const std::string& poster_url = "",
                         const std::string& overview   = "");
    void  setMatchStatus(const std::string& item_type, const std::string& kairos_id,
                         const std::string& status,   double score);
    double threshold() const;

    Database&    db_;
    ConfStore&   conf_;

    std::unique_ptr<AnidbScraper> anidb_;
    std::unique_ptr<TmdbScraper> tmdb_;
    std::unique_ptr<TvdbScraper> tvdb_;
    std::atomic<bool>            matching_{false};
};
