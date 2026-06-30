#include "SyncManager.h"
#include "EmbySource.h"
#include "JellyfinSource.h"
#include "LocalSource.h"
#include "MediaProbe.h"
#include "PlexSource.h"
#include "conf/ConfStore.h"
#include "db/ChapterRepository.h"
#include "db/Database.h"
#include "log/DebugLog.h"
#include "scraper/ScraperManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

SyncManager::SyncManager(Database& db, ConfStore& conf)
    : db_(db), conf_(conf), sync_db_(db.openConnection(60000)) {}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

namespace {
// Serialises log output so concurrent worker threads don't interleave lines.
std::mutex s_log_mu;

std::string envVar(const char* prefix, const std::string& source_id) {
    const std::string key = std::string(prefix) + source_id;
    const char* val = std::getenv(key.c_str());
    return val ? val : "";
}
} // namespace

void SyncManager::loadSources() {
    sources_.clear();
    SQLite::Statement q(sync_db_,
        "SELECT source_id, source_type, COALESCE(base_url,'') "
        "FROM media_source WHERE enabled = 1");

    while (q.executeStep()) {
        const std::string sid  = q.getColumn(0).getString();
        const std::string stype = q.getColumn(1).getString();
        const std::string surl  = q.getColumn(2).getString();
        try {
            auto src = buildSource(sid, stype, surl);
            if (src) sources_.push_back(std::move(src));
        } catch (const std::exception& e) {
            std::cerr << "[sync] failed to load source '" << sid << "': " << e.what()
                      << " — skipping (rebuild with OpenSSL for HTTPS support)\n";
        }
    }
    std::cout << "[sync] loaded " << sources_.size() << " source(s)" << std::endl;
}

// ---------------------------------------------------------------------------
// Public sync interface
// ---------------------------------------------------------------------------

void SyncManager::triggerSync(const std::string& source_id) {
    bool expected = false;
    if (!sync_running_.compare_exchange_strong(expected, true)) {
        std::cout << "[sync] already running — ignoring trigger" << std::endl;
        return;
    }
    // detach so the HTTP handler returns immediately (202)
    std::thread([this, source_id]() {
        try {
            if (source_id.empty())
                syncAll();
            else
                syncSource(source_id);
        } catch (const std::exception& e) {
            std::cerr << "[sync] error: " << e.what() << std::endl;
        }
        sync_running_.store(false);
    }).detach();
}

void SyncManager::syncAll() {
    const auto t_total = std::chrono::steady_clock::now();

    // Phase 1: ingest content from every source before running match or chapters.
    DLOG << "[sync] === phase 1: content ingestion ===\n";
    const auto t1 = std::chrono::steady_clock::now();
    for (const auto& src : sources_) {
        if (!src->isSupported()) {
            std::cout << "[sync] " << src->sourceId()
                      << " (" << src->sourceType() << ") not yet supported" << std::endl;
            continue;
        }
        syncContent(src->sourceId());
    }
    DLOG << "[sync] phase 1 done in " << elapsedMs(t1, std::chrono::steady_clock::now()) << "ms\n";

    // Phase 2: kick off scraper match in the background so it doesn't block
    // chapter sync. Network calls (TMDB/TVDB/AniDB) can be slow or unreachable;
    // running them async keeps the sync pipeline moving.
    DLOG << "[sync] === phase 2: trigger scraper match ===\n";
    if (scraper_) scraper_->triggerMatch();

    // Phase 3: chapter sync runs immediately; scraper match runs concurrently.
    DLOG << "[sync] === phase 3: chapter sync ===\n";
    const auto t3 = std::chrono::steady_clock::now();
    for (const auto& src : sources_) {
        if (src->isSupported())
            syncChaptersFromFiles(src->sourceId());
    }
    DLOG << "[sync] phase 3 done in " << elapsedMs(t3, std::chrono::steady_clock::now()) << "ms\n";

    std::cout << "[sync] all sources done (total "
              << elapsedMs(t_total, std::chrono::steady_clock::now()) << "ms)" << std::endl;
}

void SyncManager::syncContent(const std::string& source_id) {
    IMediaSource* src = findSource(source_id);
    if (!src || !src->isSupported()) return;

    std::cout << "[sync] content: " << source_id << std::endl;

    SQLite::Statement q(sync_db_,
        "SELECT library_id, external_lib_id, library_type "
        "FROM media_library WHERE source_id = ? AND enabled = 1");
    q.bind(1, source_id);

    while (q.executeStep()) {
        const std::string library_id      = q.getColumn(0).getString();
        const std::string external_lib_id = q.getColumn(1).getString();
        const std::string library_type    = q.getColumn(2).getString();

        if (library_type == "show" || library_type == "mixed")
            syncShows(*src, source_id, library_id, external_lib_id);
        if (library_type == "movie" || library_type == "mixed")
            syncMovies(*src, source_id, library_id, external_lib_id);
    }

    syncPlexLinks(source_id);
}

void SyncManager::syncSource(const std::string& source_id) {
    syncContent(source_id);
    if (scraper_) scraper_->triggerMatch();
    syncChaptersFromFiles(source_id);
    std::cout << "[sync] done: " << source_id << std::endl;
}

// ---------------------------------------------------------------------------
// Show + episode sync
// ---------------------------------------------------------------------------

namespace {
constexpr int    kEpisodeFetchConcurrency = 8;
constexpr size_t kShowMetaBatchSize       = 100; // show metadata rows per write transaction
constexpr size_t kMovieBatchSize          = 200; // movie rows per write transaction

int defaultSyncThreadCount() {
    if (const char* env = std::getenv("KAIROS_SYNC_THREADS")) {
        try {
            int n = std::stoi(env);
            if (n > 0) return n;
        } catch (const std::exception&) {}
        std::cerr << "[sync] invalid KAIROS_SYNC_THREADS value '" << env << "' — ignoring" << std::endl;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    return std::min<int>(kEpisodeFetchConcurrency, hw > 0 ? static_cast<int>(hw) : kEpisodeFetchConcurrency);
}
} // namespace

int SyncManager::getThreadCount() const {
    int ov = override_thread_count_.load(std::memory_order_relaxed);
    return ov > 0 ? ov : defaultSyncThreadCount();
}

void SyncManager::setThreadCount(int n) {
    override_thread_count_.store(n > 0 ? n : 0, std::memory_order_relaxed);
}

void SyncManager::syncShows(IMediaSource& src,
                             const std::string& source_id,
                             const std::string& library_id,
                             const std::string& external_lib_id) {
    std::cout << "[sync]   fetching shows: " << external_lib_id << std::endl;
    const auto t_shows = std::chrono::steady_clock::now();
    auto shows = src.fetchShows(external_lib_id);
    std::cout << "[sync]   " << external_lib_id << ": " << shows.size()
              << " show(s) (" << elapsedMs(t_shows, std::chrono::steady_clock::now()) << "ms)" << std::endl;
    if (shows.empty()) return; // skip stale cleanup — empty fetch may be a source error

    // Resolve IDs and upsert show metadata first so ext_show_id is captured
    // before the episode fetch below, which mutates show.show_id in place.
    // Build live_show_ids here rather than in a separate pass.
    std::vector<std::string> ext_show_ids(shows.size());
    std::vector<bool>        cross_ref_shows(shows.size(), false);
    std::unordered_set<std::string> live_show_ids;

    const std::string show_prefix = source_id + ":";
    SQLite::Statement s_resolve_show(sync_db_,
        "SELECT kairos_id FROM source_mapping "
        "WHERE item_type='show' AND source_id=? AND external_id=?");
    SQLite::Statement s_show_by_title(sync_db_,
        "SELECT show_id FROM show WHERE LOWER(title) = LOWER(?) LIMIT 1");
    SQLite::Statement s_upsert_show(sync_db_, R"(
        INSERT INTO show (show_id, title, content_rating, overview, studio, status,
                          genres, thumb, art, imdb_id, tvdb_id, tmdb_id,
                          originally_available_at, year, audience_rating,
                          labels, network, actors, countries, collections)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(show_id) DO UPDATE SET
            title                   = CASE WHEN locked THEN title                   ELSE excluded.title                   END,
            content_rating          = CASE WHEN locked THEN content_rating          ELSE excluded.content_rating          END,
            overview                = CASE WHEN locked THEN overview                ELSE excluded.overview                END,
            studio                  = CASE WHEN locked THEN studio                  ELSE excluded.studio                  END,
            status                  = CASE WHEN locked THEN status                  ELSE excluded.status                  END,
            genres                  = CASE WHEN locked THEN genres                  ELSE excluded.genres                  END,
            thumb                   = CASE WHEN locked THEN thumb                   ELSE excluded.thumb                   END,
            art                     = CASE WHEN locked THEN art                     ELSE excluded.art                     END,
            imdb_id                 = CASE WHEN locked THEN imdb_id                 ELSE excluded.imdb_id                 END,
            tvdb_id                 = CASE WHEN locked THEN tvdb_id                 ELSE excluded.tvdb_id                 END,
            tmdb_id                 = CASE WHEN locked THEN tmdb_id                 ELSE excluded.tmdb_id                 END,
            originally_available_at = CASE WHEN locked THEN originally_available_at ELSE excluded.originally_available_at END,
            year                    = CASE WHEN locked THEN year                    ELSE excluded.year                    END,
            audience_rating         = CASE WHEN locked THEN audience_rating         ELSE excluded.audience_rating         END,
            labels                  = CASE WHEN locked THEN labels                  ELSE excluded.labels                  END,
            network                 = CASE WHEN locked THEN network                 ELSE excluded.network                 END,
            actors                  = CASE WHEN locked THEN actors                  ELSE excluded.actors                  END,
            countries               = CASE WHEN locked THEN countries               ELSE excluded.countries               END,
            collections             = CASE WHEN locked THEN collections             ELSE excluded.collections             END
        WHERE NOT locked AND (
            title                   != excluded.title                   OR
            content_rating          != excluded.content_rating          OR
            overview                != excluded.overview                OR
            studio                  != excluded.studio                  OR
            status                  != excluded.status                  OR
            genres                  != excluded.genres                  OR
            thumb                   != excluded.thumb                   OR
            art                     != excluded.art                     OR
            imdb_id                 != excluded.imdb_id                 OR
            tvdb_id                 != excluded.tvdb_id                 OR
            tmdb_id                 != excluded.tmdb_id                 OR
            originally_available_at != excluded.originally_available_at OR
            COALESCE(year,           -1) != COALESCE(excluded.year,           -1) OR
            COALESCE(audience_rating, 0) != COALESCE(excluded.audience_rating,  0) OR
            labels                  != excluded.labels                  OR
            network                 != excluded.network                 OR
            actors                  != excluded.actors                  OR
            countries               != excluded.countries               OR
            collections             != excluded.collections
        )
    )");
    SQLite::Statement s_show_mapping(sync_db_, R"(
        INSERT INTO source_mapping (item_type, kairos_id, source_id, library_id, external_id)
        VALUES ('show',?,?,?,?)
        ON CONFLICT(item_type, kairos_id, source_id) DO UPDATE SET
            library_id  = excluded.library_id,
            external_id = excluded.external_id
    )");

    for (size_t batch_start = 0; batch_start < shows.size(); batch_start += kShowMetaBatchSize) {
        SQLite::Transaction txn(sync_db_);
        const size_t batch_end = std::min(batch_start + kShowMetaBatchSize, shows.size());
        for (size_t i = batch_start; i < batch_end; ++i) {
            auto& show = shows[i];
            const std::string ext_show_id = show.show_id; // source-native key before resolution
            s_resolve_show.reset();
            s_resolve_show.bind(1, source_id);
            s_resolve_show.bind(2, ext_show_id);
            std::string kairos_id = s_resolve_show.executeStep()
                ? s_resolve_show.getColumn(0).getString()
                : show_prefix + ext_show_id;
            bool is_cross_ref = false;

            // Cross-source dedup: if no existing mapping for this source, check whether
            // another source already indexes a show with the same title. If so, reuse
            // that kairos_id instead of creating a duplicate show row.
            if (kairos_id == show_prefix + ext_show_id) {
                s_show_by_title.reset();
                s_show_by_title.bind(1, show.title);
                if (s_show_by_title.executeStep()) {
                    kairos_id = s_show_by_title.getColumn(0).getString();
                    is_cross_ref = true;
                }
            } else if (!kairos_id.starts_with(show_prefix)) {
                is_cross_ref = true;  // existing mapping points to another source's item
            }

            ext_show_ids[i]    = ext_show_id;
            cross_ref_shows[i] = is_cross_ref;
            show.show_id       = kairos_id;
            live_show_ids.insert(kairos_id);

            if (!is_cross_ref) {
                s_upsert_show.reset();
                s_upsert_show.bind(1,  show.show_id);
                s_upsert_show.bind(2,  show.title);
                s_upsert_show.bind(3,  show.content_rating);
                s_upsert_show.bind(4,  show.overview);
                s_upsert_show.bind(5,  show.studio);
                s_upsert_show.bind(6,  show.status);
                s_upsert_show.bind(7,  show.genres);
                s_upsert_show.bind(8,  show.thumb);
                s_upsert_show.bind(9,  show.art);
                s_upsert_show.bind(10, show.imdb_id);
                s_upsert_show.bind(11, show.tvdb_id);
                s_upsert_show.bind(12, show.tmdb_id);
                s_upsert_show.bind(13, show.originally_available_at);
                if (show.year.has_value())            s_upsert_show.bind(14, show.year.value());
                else                                  s_upsert_show.bind(14);
                if (show.audience_rating.has_value()) s_upsert_show.bind(15, show.audience_rating.value());
                else                                  s_upsert_show.bind(15);
                s_upsert_show.bind(16, show.labels);
                s_upsert_show.bind(17, show.network);
                s_upsert_show.bind(18, show.actors);
                s_upsert_show.bind(19, show.countries);
                s_upsert_show.bind(20, show.collections);
                s_upsert_show.exec();
            }

            s_show_mapping.reset();
            s_show_mapping.bind(1, kairos_id);
            s_show_mapping.bind(2, source_id);
            s_show_mapping.bind(3, library_id);
            s_show_mapping.bind(4, ext_show_id);
            s_show_mapping.exec();
        }
        txn.commit();
        std::cout << "[sync]   wrote show metadata: "
                  << batch_end << "/" << shows.size() << std::endl;
    }

    // Episode fetches are one HTTP round-trip per show and dominate sync time,
    // so they run across a small worker pool. DB writes stay single-threaded
    // below, so the shared connection is never touched from multiple threads.
    // Requires fetchEpisodes() to tolerate concurrent calls on the same source.
    std::vector<std::vector<Episode>> episodes_by_show(shows.size());
    {
        std::atomic<size_t> next{0};
        const int worker_count = std::min<int>(getThreadCount(),
                                                static_cast<int>(shows.size()));
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(worker_count));
        for (int w = 0; w < worker_count; ++w) {
            workers.emplace_back([&]() {
                for (size_t i = next.fetch_add(1); i < shows.size(); i = next.fetch_add(1)) {
                    const auto t_fetch = std::chrono::steady_clock::now();
                    auto& eps = episodes_by_show[i] = src.fetchEpisodes(ext_show_ids[i]);
                    const long long fetch_ms = elapsedMs(t_fetch, std::chrono::steady_clock::now());

                    const auto t_validate = std::chrono::steady_clock::now();
                    for (auto& ep : eps) {
                        const std::string mapped = conf_.applyPathMap(ep.file_path);
                        const int64_t before = ep.duration_ms;
                        ep.duration_ms = validateDurationMs(ep.duration_ms, mapped);
                        if (g_debug_logging && ep.duration_ms != before) {
                            std::lock_guard lock(s_log_mu);
                            DLOG << "[sync]       duration corrected: "
                                 << before << " → " << ep.duration_ms << "ms  "
                                 << ep.file_path << '\n';
                        }
                    }
                    const long long validate_ms = elapsedMs(t_validate, std::chrono::steady_clock::now());

                    std::lock_guard lock(s_log_mu);
                    std::cout << "[sync]     \"" << shows[i].title << "\": "
                              << eps.size() << " episode(s)" << std::endl;
                    DLOG << "[sync]       fetch=" << fetch_ms << "ms validate=" << validate_ms
                         << "ms  ext_id=" << ext_show_ids[i]
                         << "  kairos_id=" << shows[i].show_id << '\n';
                }
            });
        }
        for (auto& t : workers) t.join();
    }

    {
        size_t total_eps = 0;
        for (const auto& eps : episodes_by_show) total_eps += eps.size();
        std::cout << "[sync]   writing " << shows.size() << " show(s), "
                  << total_eps << " episode(s) to db: " << external_lib_id << std::endl;
    }
    const auto t_write = std::chrono::steady_clock::now();

    // Prepare hot statements once for this library pass to avoid re-compiling SQL
    // on every episode.  For a 500-show / 15 000-episode library this replaces
    // ~60 000 sqlite3_prepare_v2 calls with 8.
    const std::string ep_prefix = source_id + ":";
    SQLite::Statement s_resolve_ep(sync_db_,
        "SELECT kairos_id FROM source_mapping "
        "WHERE item_type='episode' AND source_id=? AND external_id=?");
    SQLite::Statement s_ep_by_path(sync_db_,
        "SELECT episode_id FROM episode WHERE file_path=? LIMIT 1");
    SQLite::Statement s_clear_ep_cursors(sync_db_,
        "UPDATE media_cursor SET episode_id = NULL "
        "WHERE episode_id IN (SELECT episode_id FROM episode WHERE show_id = ?)");
    SQLite::Statement s_upsert_ep(sync_db_, R"(
        INSERT INTO episode (episode_id, show_id, season, episode, title,
                             file_path, duration_ms, overview, air_date,
                             thumb, absolute_index)
        VALUES (?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(episode_id) DO UPDATE SET
            show_id        = excluded.show_id,
            season         = excluded.season,
            episode        = excluded.episode,
            title          = CASE WHEN locked THEN title    ELSE excluded.title    END,
            file_path      = excluded.file_path,
            duration_ms    = excluded.duration_ms,
            overview       = CASE WHEN locked THEN overview ELSE excluded.overview END,
            air_date       = excluded.air_date,
            thumb          = CASE WHEN locked THEN thumb    ELSE excluded.thumb    END,
            absolute_index = excluded.absolute_index
        WHERE
            show_id        != excluded.show_id        OR
            season         != excluded.season         OR
            episode        != excluded.episode        OR
            file_path      != excluded.file_path      OR
            duration_ms    != excluded.duration_ms    OR
            air_date       != excluded.air_date       OR
            COALESCE(absolute_index, -1) != COALESCE(excluded.absolute_index, -1) OR
            (NOT locked AND (
                title    != excluded.title    OR
                overview != excluded.overview OR
                thumb    != excluded.thumb
            ))
    )");
    SQLite::Statement s_ep_mapping(sync_db_, R"(
        INSERT INTO source_mapping (item_type, kairos_id, source_id, library_id, external_id)
        VALUES ('episode',?,?,?,?)
        ON CONFLICT(item_type, kairos_id, source_id) DO UPDATE SET
            library_id  = excluded.library_id,
            external_id = excluded.external_id
    )");
    SQLite::Statement s_delete_seasons(sync_db_,
        "DELETE FROM show_season WHERE show_id = ?");
    SQLite::Statement s_insert_season(sync_db_,
        "INSERT INTO show_season (show_id, season, season_name) VALUES (?,?,?)");

    auto ep_resolve_id = [&](const std::string& ext) -> std::string {
        s_resolve_ep.reset();
        s_resolve_ep.bind(1, source_id);
        s_resolve_ep.bind(2, ext);
        return s_resolve_ep.executeStep() ? s_resolve_ep.getColumn(0).getString() : ep_prefix + ext;
    };
    auto ep_resolve_by_path = [&](const std::string& path) -> std::string {
        if (path.empty()) return "";
        s_ep_by_path.reset();
        s_ep_by_path.bind(1, path);
        return s_ep_by_path.executeStep() ? s_ep_by_path.getColumn(0).getString() : "";
    };

    // Each show gets its own transaction so a single bad episode or duplicate
    // doesn't roll back the entire library.  Per-episode errors are logged and
    // skipped rather than aborting the show.
    for (size_t i = 0; i < shows.size(); ++i) {
        auto& show      = shows[i];
        auto& episodes  = episodes_by_show[i];
        const bool cross_show = cross_ref_shows[i];

        yieldIfRequested();

        try {
            SQLite::Transaction txn(sync_db_);

            std::unordered_map<int, std::string> season_names;
            std::unordered_set<std::string> live_ep_ids;
            for (auto& ep : episodes) {
                const std::string ext_ep_id = ep.episode_id;
                bool ep_cross_ref = false;
                std::string ep_kairos_id;

                if (cross_show) {
                    ep_kairos_id = ep_resolve_by_path(ep.file_path);
                    if (!ep_kairos_id.empty()) {
                        ep_cross_ref = true;
                    } else {
                        ep_kairos_id = ep_resolve_id(ext_ep_id);
                    }
                } else {
                    ep_kairos_id = ep_resolve_id(ext_ep_id);
                    if (ep_kairos_id == ep_prefix + ext_ep_id) {
                        const std::string existing = ep_resolve_by_path(ep.file_path);
                        if (!existing.empty()) { ep_kairos_id = existing; ep_cross_ref = true; }
                    } else if (!ep_kairos_id.starts_with(ep_prefix)) {
                        ep_cross_ref = true;
                    }
                }

                ep.episode_id = ep_kairos_id;
                ep.show_id    = show.show_id;

                try {
                    if (!ep_cross_ref) {
                        live_ep_ids.insert(ep.episode_id);
                        s_upsert_ep.reset();
                        s_upsert_ep.bind(1,  ep.episode_id);
                        s_upsert_ep.bind(2,  ep.show_id);
                        s_upsert_ep.bind(3,  ep.season);
                        s_upsert_ep.bind(4,  ep.episode);
                        s_upsert_ep.bind(5,  ep.title);
                        s_upsert_ep.bind(6,  ep.file_path);
                        s_upsert_ep.bind(7,  ep.duration_ms);
                        s_upsert_ep.bind(8,  ep.overview);
                        s_upsert_ep.bind(9,  ep.air_date);
                        s_upsert_ep.bind(10, ep.thumb);
                        if (ep.absolute_index.has_value()) s_upsert_ep.bind(11, ep.absolute_index.value());
                        else                               s_upsert_ep.bind(11);
                        s_upsert_ep.exec();
                    }
                    s_ep_mapping.reset();
                    s_ep_mapping.bind(1, ep.episode_id);
                    s_ep_mapping.bind(2, source_id);
                    s_ep_mapping.bind(3, library_id);
                    s_ep_mapping.bind(4, ext_ep_id);
                    s_ep_mapping.exec();
                } catch (const std::exception& e) {
                    std::cerr << "[sync] skipping episode " << ep.file_path
                              << ": " << e.what() << '\n';
                }

                if (!ep.season_name.empty() && !season_names.count(ep.season))
                    season_names[ep.season] = ep.season_name;
            }

            // Remove episodes that were in the DB but are no longer returned by
            // the source. Nullify cursor refs first (RESTRICT FK on episode_id).
            if (!cross_show) {
                SQLite::Statement q_existing(sync_db_,
                    "SELECT episode_id FROM episode WHERE show_id = ?");
                q_existing.bind(1, show.show_id);
                std::vector<std::string> stale_eps;
                while (q_existing.executeStep()) {
                    const std::string eid = q_existing.getColumn(0).getString();
                    if (!live_ep_ids.contains(eid)) stale_eps.push_back(eid);
                }
                for (const auto& eid : stale_eps) {
                    s_clear_ep_cursors.reset(); s_clear_ep_cursors.bind(1, show.show_id); s_clear_ep_cursors.exec();
                    SQLite::Statement d(sync_db_, "DELETE FROM episode WHERE episode_id = ?");
                    d.bind(1, eid); d.exec();
                }

                s_delete_seasons.reset(); s_delete_seasons.bind(1, show.show_id); s_delete_seasons.exec();
                for (const auto& [season, name] : season_names) {
                    s_insert_season.reset();
                    s_insert_season.bind(1, show.show_id);
                    s_insert_season.bind(2, season);
                    s_insert_season.bind(3, name);
                    s_insert_season.exec();
                }
            }

            txn.commit();
            std::cout << "[sync]   wrote series: \"" << show.title << "\" ("
                      << episodes.size() << " episode(s))" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[sync] error syncing show \"" << shows[i].title
                      << "\": " << e.what() << " — skipping\n";
        }
    }

    std::cout << "[sync]   episodes done: " << external_lib_id
              << " (" << elapsedMs(t_write, std::chrono::steady_clock::now()) << "ms)" << std::endl;

    // Stale show cleanup runs outside per-show transactions.
    try {
        SQLite::Transaction txn(sync_db_);

        std::vector<std::string> stale_shows;
        {
            SQLite::Statement q(sync_db_,
                "SELECT kairos_id FROM source_mapping "
                "WHERE item_type='show' AND source_id=? AND library_id=?");
            q.bind(1, source_id); q.bind(2, library_id);
            while (q.executeStep()) {
                const std::string kid = q.getColumn(0).getString();
                if (!live_show_ids.contains(kid)) stale_shows.push_back(kid);
            }
        }
        for (const auto& kid : stale_shows) {
            bool other_source_has_it = false;
            {
                SQLite::Statement chk(sync_db_,
                    "SELECT COUNT(*) FROM source_mapping "
                    "WHERE item_type='show' AND kairos_id=? AND source_id!=?");
                chk.bind(1, kid); chk.bind(2, source_id);
                other_source_has_it = chk.executeStep() && chk.getColumn(0).getInt() > 0;
            }
            if (!other_source_has_it) {
                { SQLite::Statement q(sync_db_,
                      "UPDATE media_cursor SET episode_id = NULL "
                      "WHERE episode_id IN (SELECT episode_id FROM episode WHERE show_id = ?)");
                  q.bind(1, kid); q.exec(); }
                { SQLite::Statement d(sync_db_, "DELETE FROM episode WHERE show_id = ?");
                  d.bind(1, kid); d.exec(); }
                { SQLite::Statement d(sync_db_, "DELETE FROM show WHERE show_id=?");
                  d.bind(1, kid); d.exec(); }
                std::cout << "[sync]   removed stale show: " << kid << std::endl;
            }
            { SQLite::Statement d(sync_db_,
                  "DELETE FROM source_mapping WHERE item_type='show' AND kairos_id=? AND source_id=? AND library_id=?");
              d.bind(1, kid); d.bind(2, source_id); d.bind(3, library_id); d.exec(); }
        }

        { SQLite::Statement d(sync_db_,
              "DELETE FROM source_mapping WHERE item_type='show' AND source_id=? AND library_id=?"
              " AND kairos_id NOT IN (SELECT show_id FROM show)");
          d.bind(1, source_id); d.bind(2, library_id); d.exec(); }
        { SQLite::Statement d(sync_db_,
              "DELETE FROM source_mapping WHERE item_type='episode' AND source_id=? AND library_id=?"
              " AND kairos_id NOT IN (SELECT episode_id FROM episode)");
          d.bind(1, source_id); d.bind(2, library_id); d.exec(); }

        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[sync] error during stale show cleanup: " << e.what() << " — skipping\n";
    }
}

// ---------------------------------------------------------------------------
// Movie sync
// ---------------------------------------------------------------------------

void SyncManager::syncMovies(IMediaSource& src,
                              const std::string& source_id,
                              const std::string& library_id,
                              const std::string& external_lib_id) {
    std::cout << "[sync]   fetching movies: " << external_lib_id << std::endl;
    const auto t_movies = std::chrono::steady_clock::now();
    auto movies = src.fetchMovies(external_lib_id);
    std::cout << "[sync]   " << external_lib_id << ": " << movies.size()
              << " movie(s) (" << elapsedMs(t_movies, std::chrono::steady_clock::now()) << "ms)" << std::endl;
    if (movies.empty()) return;

    // Resolve durations before touching the DB so file I/O doesn't happen
    // inside a write transaction.
    for (auto& movie : movies)
        movie.duration_ms = validateDurationMs(movie.duration_ms, conf_.applyPathMap(movie.file_path));

    // Process in batches. sync_db_ is a dedicated connection so HTTP writes on
    // the main connection are only blocked for the duration of each batch's
    // write transaction (SQLite WAL busy_timeout handles the wait).
    std::unordered_set<std::string> live_movie_ids;
    const auto t_write = std::chrono::steady_clock::now();

    const std::string movie_prefix = source_id + ":";
    SQLite::Statement s_resolve_movie(sync_db_,
        "SELECT kairos_id FROM source_mapping "
        "WHERE item_type='movie' AND source_id=? AND external_id=?");
    SQLite::Statement s_movie_by_path(sync_db_,
        "SELECT movie_id FROM movie WHERE file_path=? LIMIT 1");
    SQLite::Statement s_upsert_movie(sync_db_, R"(
        INSERT INTO movie (movie_id, title, content_rating, file_path, duration_ms, year,
                           overview, tagline, studio, director, genres, thumb, art,
                           imdb_id, tmdb_id, audience_rating,
                           labels, actors, countries, collections)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(movie_id) DO UPDATE SET
            title           = CASE WHEN locked THEN title           ELSE excluded.title           END,
            content_rating  = CASE WHEN locked THEN content_rating  ELSE excluded.content_rating  END,
            file_path       = CASE WHEN locked THEN file_path       ELSE excluded.file_path       END,
            duration_ms     = CASE WHEN locked THEN duration_ms     ELSE excluded.duration_ms     END,
            year            = CASE WHEN locked THEN year            ELSE excluded.year            END,
            overview        = CASE WHEN locked THEN overview        ELSE excluded.overview        END,
            tagline         = CASE WHEN locked THEN tagline         ELSE excluded.tagline         END,
            studio          = CASE WHEN locked THEN studio          ELSE excluded.studio          END,
            director        = CASE WHEN locked THEN director        ELSE excluded.director        END,
            genres          = CASE WHEN locked THEN genres          ELSE excluded.genres          END,
            thumb           = CASE WHEN locked THEN thumb           ELSE excluded.thumb           END,
            art             = CASE WHEN locked THEN art             ELSE excluded.art             END,
            imdb_id         = CASE WHEN locked THEN imdb_id         ELSE excluded.imdb_id         END,
            tmdb_id         = CASE WHEN locked THEN tmdb_id         ELSE excluded.tmdb_id         END,
            audience_rating = CASE WHEN locked THEN audience_rating ELSE excluded.audience_rating END,
            labels          = CASE WHEN locked THEN labels          ELSE excluded.labels          END,
            actors          = CASE WHEN locked THEN actors          ELSE excluded.actors          END,
            countries       = CASE WHEN locked THEN countries       ELSE excluded.countries       END,
            collections     = CASE WHEN locked THEN collections     ELSE excluded.collections     END
        WHERE NOT locked AND (
            title           != excluded.title           OR
            content_rating  != excluded.content_rating  OR
            file_path       != excluded.file_path       OR
            duration_ms     != excluded.duration_ms     OR
            overview        != excluded.overview        OR
            tagline         != excluded.tagline         OR
            studio          != excluded.studio          OR
            director        != excluded.director        OR
            genres          != excluded.genres          OR
            thumb           != excluded.thumb           OR
            art             != excluded.art             OR
            imdb_id         != excluded.imdb_id         OR
            tmdb_id         != excluded.tmdb_id         OR
            COALESCE(year,           -1) != COALESCE(excluded.year,           -1) OR
            COALESCE(audience_rating, 0) != COALESCE(excluded.audience_rating,  0) OR
            labels          != excluded.labels          OR
            actors          != excluded.actors          OR
            countries       != excluded.countries       OR
            collections     != excluded.collections
        )
    )");
    SQLite::Statement s_movie_mapping(sync_db_, R"(
        INSERT INTO source_mapping (item_type, kairos_id, source_id, library_id, external_id)
        VALUES ('movie',?,?,?,?)
        ON CONFLICT(item_type, kairos_id, source_id) DO UPDATE SET
            library_id  = excluded.library_id,
            external_id = excluded.external_id
    )");

    for (size_t batch_start = 0; batch_start < movies.size(); batch_start += kMovieBatchSize) {
        yieldIfRequested();
        const size_t batch_end = std::min(batch_start + kMovieBatchSize, movies.size());

        try {
        SQLite::Transaction txn(sync_db_);
        for (size_t mi = batch_start; mi < batch_end; ++mi) {
            auto& movie = movies[mi];
            const std::string ext_movie_id = movie.movie_id;
            s_resolve_movie.reset();
            s_resolve_movie.bind(1, source_id);
            s_resolve_movie.bind(2, ext_movie_id);
            std::string kairos_id = s_resolve_movie.executeStep()
                ? s_resolve_movie.getColumn(0).getString()
                : movie_prefix + ext_movie_id;
            bool is_cross_ref = false;

            // Cross-source dedup: if no existing mapping for this source, check whether
            // another source already indexes this file_path to avoid duplicate movie rows.
            if (kairos_id == movie_prefix + ext_movie_id) {
                s_movie_by_path.reset();
                s_movie_by_path.bind(1, movie.file_path);
                if (s_movie_by_path.executeStep()) {
                    kairos_id = s_movie_by_path.getColumn(0).getString();
                    is_cross_ref = true;
                }
            } else if (!kairos_id.starts_with(movie_prefix)) {
                is_cross_ref = true;
            }

            movie.movie_id = kairos_id;
            live_movie_ids.insert(movie.movie_id);

            if (!is_cross_ref) {
                s_upsert_movie.reset();
                s_upsert_movie.bind(1,  movie.movie_id);
                s_upsert_movie.bind(2,  movie.title);
                s_upsert_movie.bind(3,  movie.content_rating);
                s_upsert_movie.bind(4,  movie.file_path);
                s_upsert_movie.bind(5,  movie.duration_ms);
                if (movie.year.has_value())            s_upsert_movie.bind(6,  movie.year.value());
                else                                   s_upsert_movie.bind(6);
                s_upsert_movie.bind(7,  movie.overview);
                s_upsert_movie.bind(8,  movie.tagline);
                s_upsert_movie.bind(9,  movie.studio);
                s_upsert_movie.bind(10, movie.director);
                s_upsert_movie.bind(11, movie.genres);
                s_upsert_movie.bind(12, movie.thumb);
                s_upsert_movie.bind(13, movie.art);
                s_upsert_movie.bind(14, movie.imdb_id);
                s_upsert_movie.bind(15, movie.tmdb_id);
                if (movie.audience_rating.has_value()) s_upsert_movie.bind(16, movie.audience_rating.value());
                else                                   s_upsert_movie.bind(16);
                s_upsert_movie.bind(17, movie.labels);
                s_upsert_movie.bind(18, movie.actors);
                s_upsert_movie.bind(19, movie.countries);
                s_upsert_movie.bind(20, movie.collections);
                s_upsert_movie.exec();
            }

            s_movie_mapping.reset();
            s_movie_mapping.bind(1, movie.movie_id);
            s_movie_mapping.bind(2, source_id);
            s_movie_mapping.bind(3, library_id);
            s_movie_mapping.bind(4, ext_movie_id);
            s_movie_mapping.exec();
        }
        txn.commit();
        std::cout << "[sync]   wrote movies: "
                  << batch_end << "/" << movies.size() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[sync] error writing movie batch "
                      << batch_start << "-" << batch_end
                      << ": " << e.what() << " — skipping\n";
        }
    }

    std::cout << "[sync]   movies done: " << external_lib_id
              << " (" << elapsedMs(t_write, std::chrono::steady_clock::now()) << "ms)" << std::endl;

    // Stale cleanup runs in its own transaction after all upserts are done.
    yieldIfRequested();
    {
        std::vector<std::string> stale_movies;
        {
            SQLite::Statement q(sync_db_,
                "SELECT kairos_id FROM source_mapping "
                "WHERE item_type='movie' AND source_id=? AND library_id=?");
            q.bind(1, source_id); q.bind(2, library_id);
            while (q.executeStep()) {
                const std::string kid = q.getColumn(0).getString();
                if (!live_movie_ids.contains(kid)) stale_movies.push_back(kid);
            }
        }

        try {
            SQLite::Transaction txn(sync_db_);
            for (const auto& kid : stale_movies) {
                bool other_source_has_it = false;
                {
                    SQLite::Statement chk(sync_db_,
                        "SELECT COUNT(*) FROM source_mapping "
                        "WHERE item_type='movie' AND kairos_id=? AND source_id!=?");
                    chk.bind(1, kid); chk.bind(2, source_id);
                    other_source_has_it = chk.executeStep() && chk.getColumn(0).getInt() > 0;
                }
                if (!other_source_has_it) {
                    SQLite::Statement d(sync_db_, "DELETE FROM movie WHERE movie_id=?");
                    d.bind(1, kid); d.exec();
                    std::cout << "[sync]   removed stale movie: " << kid << std::endl;
                }
                { SQLite::Statement d(sync_db_,
                      "DELETE FROM source_mapping WHERE item_type='movie' AND kairos_id=? AND source_id=? AND library_id=?");
                  d.bind(1, kid); d.bind(2, source_id); d.bind(3, library_id); d.exec(); }
            }

            { SQLite::Statement d(sync_db_,
                  "DELETE FROM source_mapping WHERE item_type='movie' AND source_id=? AND library_id=?"
                  " AND kairos_id NOT IN (SELECT movie_id FROM movie)");
              d.bind(1, source_id); d.bind(2, library_id); d.exec(); }

            txn.commit();
        } catch (const std::exception& e) {
            std::cerr << "[sync] error during stale movie cleanup: " << e.what() << " — skipping\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string SyncManager::resolveId(const std::string& item_type,
                                    const std::string& source_id,
                                    const std::string& external_id) const {
    SQLite::Statement q(sync_db_,
        "SELECT kairos_id FROM source_mapping "
        "WHERE item_type = ? AND source_id = ? AND external_id = ?");
    q.bind(1, item_type);
    q.bind(2, source_id);
    q.bind(3, external_id);
    if (q.executeStep())
        return q.getColumn(0).getString();
    return source_id + ":" + external_id; // deterministic, no UUID needed
}

std::string SyncManager::resolveByFilePath(const std::string& item_type,
                                            const std::string& file_path) const {
    if (file_path.empty()) return "";
    const bool is_ep = (item_type == "episode");
    const std::string col = is_ep ? "episode_id" : "movie_id";
    const std::string tbl = is_ep ? "episode"    : "movie";
    SQLite::Statement q(sync_db_,
        "SELECT " + col + " FROM " + tbl + " WHERE file_path = ? LIMIT 1");
    q.bind(1, file_path);
    return q.executeStep() ? q.getColumn(0).getString() : "";
}

std::string SyncManager::resolveByTitle(const std::string& item_type,
                                         const std::string& title) const {
    if (title.empty()) return "";
    const bool is_show = (item_type == "show");
    const std::string col = is_show ? "show_id" : "movie_id";
    const std::string tbl = is_show ? "show"    : "movie";
    SQLite::Statement q(sync_db_,
        "SELECT " + col + " FROM " + tbl + " WHERE LOWER(title) = LOWER(?) LIMIT 1");
    q.bind(1, title);
    return q.executeStep() ? q.getColumn(0).getString() : "";
}

void SyncManager::upsertMapping(const std::string& item_type,
                                 const std::string& kairos_id,
                                 const std::string& source_id,
                                 const std::string& library_id,
                                 const std::string& external_id) {
    SQLite::Statement s(sync_db_, R"(
        INSERT INTO source_mapping (item_type, kairos_id, source_id, library_id, external_id)
        VALUES (?,?,?,?,?)
        ON CONFLICT(item_type, kairos_id, source_id) DO UPDATE SET
            library_id  = excluded.library_id,
            external_id = excluded.external_id
    )");
    s.bind(1, item_type);
    s.bind(2, kairos_id);
    s.bind(3, source_id);
    s.bind(4, library_id);
    s.bind(5, external_id);
    s.exec();
}

std::vector<std::string> SyncManager::sourceIds() const {
    std::vector<std::string> ids;
    ids.reserve(sources_.size());
    for (const auto& s : sources_) ids.push_back(s->sourceId());
    return ids;
}

// ---------------------------------------------------------------------------
// Coordinator yield
// ---------------------------------------------------------------------------

void SyncManager::yieldIfRequested() {
    if (!yield_requested_.load(std::memory_order_relaxed)) return;
    DLOG << "[sync] yielding — coordinator requested DB write window\n";
    while (yield_requested_.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

// ---------------------------------------------------------------------------
// Plex-linked list sync
// ---------------------------------------------------------------------------

void SyncManager::triggerPlexLinkSync() {
    bool expected = false;
    if (!plex_sync_running_.compare_exchange_strong(expected, true)) {
        std::cout << "[sync] plex-link sync already running — ignoring trigger" << std::endl;
        return;
    }
    std::thread([this]() {
        try {
            for (const auto& src : sources_)
                syncPlexLinks(src->sourceId());
        } catch (const std::exception& e) {
            std::cerr << "[sync] plex-link sync error: " << e.what() << std::endl;
        }
        plex_sync_running_.store(false);
    }).detach();
}

void SyncManager::syncPlexLinks(const std::string& source_id) {
    // Only Plex sources have playlists/collections
    std::string base_url, source_type;
    {
        SQLite::Statement sq(sync_db_,
            "SELECT base_url, source_type FROM media_source WHERE source_id = ? AND enabled = 1");
        sq.bind(1, source_id);
        if (!sq.executeStep()) return;
        base_url    = sq.getColumn(0).getString();
        source_type = sq.getColumn(1).getString();
    }
    if (source_type != "plex" || base_url.empty()) return;

    std::string token = conf_.token(source_id);
    if (token.empty()) return;

    struct LinkRow { std::string list_type, list_id, external_id, plex_type; };
    std::vector<LinkRow> links;
    {
        SQLite::Statement q(sync_db_,
            "SELECT list_type, list_id, external_id, plex_type "
            "FROM plex_list_link WHERE source_id = ?");
        q.bind(1, source_id);
        while (q.executeStep()) {
            links.push_back({
                q.getColumn(0).getString(), q.getColumn(1).getString(),
                q.getColumn(2).getString(), q.getColumn(3).getString()
            });
        }
    }
    if (links.empty()) return;

    std::cout << "[sync] re-syncing " << links.size() << " Plex-linked list(s)" << std::endl;

    httplib::Client client(base_url);
    client.set_default_headers({{"X-Plex-Token", token}, {"Accept", "application/json"}});
    client.set_connection_timeout(10);
    client.set_read_timeout(30);

    for (const auto& link : links) {
        try {
            std::string path = (link.plex_type == "playlist")
                ? "/playlists/" + link.external_id + "/items"
                : "/library/metadata/" + link.external_id + "/children";

            auto r = client.Get(path.c_str());
            if (!r || r->status != 200) {
                std::cerr << "[sync] failed to fetch Plex items for list "
                          << link.list_id << " (HTTP " << (r ? r->status : 0) << ")" << std::endl;
                continue;
            }

            struct Item { std::string item_type; std::string kairos_id; };
            std::vector<Item> items;
            auto j = json::parse(r->body);
            const auto& md = j["MediaContainer"];
            if (md.contains("Metadata")) {
                for (const auto& item : md["Metadata"]) {
                    std::string pt = item.value("type", "");
                    std::string it = (pt == "movie") ? "movie" : "episode";
                    std::string rk = item["ratingKey"].get<std::string>();
                    SQLite::Statement lk(sync_db_,
                        "SELECT kairos_id FROM source_mapping "
                        "WHERE source_id=? AND external_id=? AND item_type=?");
                    lk.bind(1, source_id); lk.bind(2, rk); lk.bind(3, it);
                    if (lk.executeStep()) items.push_back({ it, lk.getColumn(0).getString() });
                }
            }

            const std::string fk_col   = (link.list_type == "playlist") ? "playlist_id"    : "filler_list_id";
            const std::string item_tbl = (link.list_type == "playlist") ? "playlist_item"  : "filler_list_item";

            SQLite::Transaction txn(sync_db_);
            SQLite::Statement del(sync_db_,
                "DELETE FROM " + item_tbl + " WHERE " + fk_col + " = ?");
            del.bind(1, link.list_id); del.exec();

            int pos = 0;
            for (const auto& item : items) {
                SQLite::Statement ins(sync_db_,
                    "INSERT OR IGNORE INTO " + item_tbl +
                    " (" + fk_col + ", position, item_type, item_id) VALUES (?,?,?,?)");
                ins.bind(1, link.list_id); ins.bind(2, pos++);
                ins.bind(3, item.item_type); ins.bind(4, item.kairos_id);
                ins.exec();
            }

            SQLite::Statement ts(sync_db_,
                "UPDATE plex_list_link SET last_synced_at = ? WHERE list_type = ? AND list_id = ?");
            ts.bind(1, static_cast<int64_t>(std::time(nullptr)));
            ts.bind(2, link.list_type); ts.bind(3, link.list_id);
            ts.exec();

            txn.commit();
            std::cout << "[sync]   \"" << link.list_id << "\": "
                      << items.size() << " item(s)" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[sync] error syncing list " << link.list_id
                      << ": " << e.what() << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------

IMediaSource* SyncManager::findSource(const std::string& source_id) const {
    for (const auto& s : sources_)
        if (s->sourceId() == source_id) return s.get();
    return nullptr;
}

std::unique_ptr<IMediaSource> SyncManager::buildSource(const std::string& source_id,
                                                       const std::string& source_type,
                                                       const std::string& base_url) const {
    // Conf file takes precedence; env vars are the fallback for manual setups
    std::string token   = conf_.token(source_id);
    std::string user_id = conf_.userId(source_id);
    if (token.empty())   token   = envVar("KAIROS_TOKEN_",   source_id);
    if (user_id.empty()) user_id = envVar("KAIROS_USER_ID_", source_id);

    if (source_type == "plex") {
        if (token.empty()) {
            std::cout << "[sync] no token for " << source_id
                      << " (set via UI or KAIROS_TOKEN_" << source_id << ") — skipping" << std::endl;
            return nullptr;
        }
        return std::make_unique<PlexSource>(source_id, base_url, token);
    }
    if (source_type == "jellyfin")
        return std::make_unique<JellyfinSource>(source_id, base_url, token, user_id);
    if (source_type == "emby")
        return std::make_unique<EmbySource>(source_id, base_url, token, user_id);
    if (source_type == "local")
        return std::make_unique<LocalSource>(source_id, base_url);

    std::cout << "[sync] unknown source type '" << source_type << "' — skipping" << std::endl;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Chapter sync
// ---------------------------------------------------------------------------

void SyncManager::syncItemChapters(IMediaSource& src,
                                    const std::string& media_type,
                                    const std::string& kairos_id,
                                    const std::string& external_id,
                                    const std::string& file_path) {
    ChapterRepository repo(db_);

    // Typed markers (intro, credits) from source API — highest priority.
    if (!external_id.empty()) {
        auto intro = src.fetchIntroMarkers(external_id);
        if (!intro.empty())
            repo.syncChapters(media_type, kairos_id, "plex_intro", std::move(intro));

        auto source_ch = src.fetchChapters(external_id);
        if (!source_ch.empty())
            repo.syncChapters(media_type, kairos_id, "plex_chapters", std::move(source_ch));
    }

    // File-embedded chapters via ffprobe.
    if (!file_path.empty()) {
        auto file_ch = probeChapters(conf_.applyPathMap(file_path));
        if (!file_ch.empty())
            repo.syncChapters(media_type, kairos_id, "file", std::move(file_ch));
    }
}

void SyncManager::syncChaptersFromFiles(const std::string& source_id) {
    {
        SQLite::Statement q(sync_db_,
            "SELECT value FROM app_config WHERE key='chapter_sync_enabled'");
        if (q.executeStep() && q.getColumn(0).getString() == "false") return;
    }

    const auto t_ch_start = std::chrono::steady_clock::now();
    std::cout << "[sync] chapter sync (file): " << source_id << std::endl;
    ChapterRepository repo(db_);

    int ep_checked = 0, ep_skipped_duration = 0, ep_skipped_missing = 0, ep_with_chapters = 0;

    // Episodes
    {
        SQLite::Statement q(sync_db_, R"(
            SELECT sm.kairos_id, e.file_path, e.show_id, sh.title, e.duration_ms
            FROM source_mapping sm
            JOIN episode e  ON e.episode_id = sm.kairos_id
            JOIN show    sh ON sh.show_id   = e.show_id
            WHERE sm.item_type='episode' AND sm.source_id=?
            ORDER BY e.show_id
        )");
        q.bind(1, source_id);
        std::string cur_show_id;
        while (q.executeStep()) {
            const std::string kairos_id  = q.getColumn(0).getString();
            const std::string file_path  = q.getColumn(1).getString();
            const std::string show_id    = q.getColumn(2).getString();
            const std::string show_title = q.getColumn(3).getString();
            const int64_t     duration   = q.getColumn(4).getInt64();
            if (show_id != cur_show_id) {
                cur_show_id = show_id;
                std::cout << "[sync]   checking chapters: \"" << show_title << "\"" << std::endl;
            }
            ++ep_checked;

            // Skip files that failed duration probing — ffprobe can't read them.
            if (file_path.empty() || duration == 0) {
                ++ep_skipped_duration;
                DLOG << "[sync]     skip (no path/duration=0): kairos_id=" << kairos_id
                     << "  path=" << file_path << "  duration=" << duration << '\n';
                continue;
            }
            const std::string mapped_path = conf_.applyPathMap(file_path);
            const bool exists = std::filesystem::exists(mapped_path);
            DLOG << "[sync]     ep kairos_id=" << kairos_id
                 << "\n            src:    " << file_path
                 << "\n            mapped: " << mapped_path
                 << "\n            exists: " << (exists ? "yes" : "NO")
                 << "  duration=" << duration << "ms\n";
            if (!exists) {
                ++ep_skipped_missing;
                continue;
            }
            const auto t_probe = std::chrono::steady_clock::now();
            auto chapters = probeChapters(mapped_path);
            DLOG << "[sync]     probe took " << elapsedMs(t_probe, std::chrono::steady_clock::now())
                 << "ms → " << chapters.size() << " chapter(s)\n";
            if (!chapters.empty()) {
                ++ep_with_chapters;
                repo.syncChapters("episode", kairos_id, "file", std::move(chapters));
            }
        }
    }

    DLOG << "[sync] episode chapter summary: checked=" << ep_checked
         << " skipped_duration=" << ep_skipped_duration
         << " skipped_missing=" << ep_skipped_missing
         << " with_chapters=" << ep_with_chapters << '\n';

    int mv_checked = 0, mv_skipped_missing = 0, mv_with_chapters = 0;

    // Movies
    {
        SQLite::Statement q(sync_db_, R"(
            SELECT sm.kairos_id, m.file_path
            FROM source_mapping sm
            JOIN movie m ON m.movie_id = sm.kairos_id
            WHERE sm.item_type='movie' AND sm.source_id=?
        )");
        q.bind(1, source_id);
        while (q.executeStep()) {
            const std::string kairos_id = q.getColumn(0).getString();
            const std::string file_path = q.getColumn(1).getString();
            if (file_path.empty()) continue;
            ++mv_checked;
            const std::string mapped_path = conf_.applyPathMap(file_path);
            const bool exists = std::filesystem::exists(mapped_path);
            DLOG << "[sync]   movie kairos_id=" << kairos_id
                 << "\n           src:    " << file_path
                 << "\n           mapped: " << mapped_path
                 << "\n           exists: " << (exists ? "yes" : "NO") << '\n';
            if (!exists) {
                ++mv_skipped_missing;
                continue;
            }
            const auto t_probe = std::chrono::steady_clock::now();
            auto chapters = probeChapters(mapped_path);
            DLOG << "[sync]   probe took " << elapsedMs(t_probe, std::chrono::steady_clock::now())
                 << "ms → " << chapters.size() << " chapter(s)\n";
            if (!chapters.empty()) {
                ++mv_with_chapters;
                repo.syncChapters("movie", kairos_id, "file", std::move(chapters));
            }
        }
    }

    DLOG << "[sync] movie chapter summary: checked=" << mv_checked
         << " skipped_missing=" << mv_skipped_missing
         << " with_chapters=" << mv_with_chapters << '\n';

    std::cout << "[sync] chapter sync done: " << source_id
              << " (" << elapsedMs(t_ch_start, std::chrono::steady_clock::now()) << "ms)" << std::endl;
}
