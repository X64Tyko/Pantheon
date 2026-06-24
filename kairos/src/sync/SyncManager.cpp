#include "SyncManager.h"
#include "EmbySource.h"
#include "JellyfinSource.h"
#include "LocalSource.h"
#include "MediaProbe.h"
#include "PlexSource.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

SyncManager::SyncManager(Database& db, ConfStore& conf) : db_(db), conf_(conf) {}

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
    SQLite::Statement q(db_.get(),
        "SELECT source_id, source_type, COALESCE(base_url,'') "
        "FROM media_source WHERE enabled = 1");

    while (q.executeStep()) {
        auto src = buildSource(q.getColumn(0).getString(),
                               q.getColumn(1).getString(),
                               q.getColumn(2).getString());
        if (src) sources_.push_back(std::move(src));
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
    for (const auto& src : sources_) {
        if (!src->isSupported()) {
            std::cout << "[sync] " << src->sourceId()
                      << " (" << src->sourceType() << ") not yet supported" << std::endl;
            continue;
        }
        syncSource(src->sourceId());
    }
}

void SyncManager::syncSource(const std::string& source_id) {
    MediaSource* src = findSource(source_id);
    if (!src || !src->isSupported()) return;

    std::cout << "[sync] syncing source: " << source_id << std::endl;

    SQLite::Statement q(db_.get(),
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
    std::cout << "[sync] done: " << source_id << std::endl;
}

// ---------------------------------------------------------------------------
// Show + episode sync
// ---------------------------------------------------------------------------

namespace {
constexpr int kEpisodeFetchConcurrency = 8;

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

void SyncManager::syncShows(MediaSource& src,
                             const std::string& source_id,
                             const std::string& library_id,
                             const std::string& external_lib_id) {
    auto shows = src.fetchShows(external_lib_id);
    std::cout << "[sync]   " << external_lib_id << ": " << shows.size() << " show(s)" << std::endl;
    if (shows.empty()) return; // skip stale cleanup — empty fetch may be a source error

    // Resolve IDs and upsert show metadata first so ext_show_id is captured
    // before the episode fetch below, which mutates show.show_id in place.
    // Build live_show_ids here rather than in a separate pass.
    std::vector<std::string> ext_show_ids(shows.size());
    std::unordered_set<std::string> live_show_ids;
    {
        SQLite::Transaction txn(db_.get());
        for (size_t i = 0; i < shows.size(); ++i) {
            auto& show = shows[i];
            const std::string ext_show_id = show.show_id; // Plex rating key before resolution
            const std::string kairos_id   = resolveId("show", source_id, ext_show_id);
            ext_show_ids[i] = ext_show_id;
            show.show_id    = kairos_id;
            live_show_ids.insert(kairos_id);

            SQLite::Statement s(db_.get(), R"(
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
            )");
            s.bind(1,  show.show_id);
            s.bind(2,  show.title);
            s.bind(3,  show.content_rating);
            s.bind(4,  show.overview);
            s.bind(5,  show.studio);
            s.bind(6,  show.status);
            s.bind(7,  show.genres);
            s.bind(8,  show.thumb);
            s.bind(9,  show.art);
            s.bind(10, show.imdb_id);
            s.bind(11, show.tvdb_id);
            s.bind(12, show.tmdb_id);
            s.bind(13, show.originally_available_at);
            if (show.year.has_value())            s.bind(14, show.year.value());
            else                                  s.bind(14);
            if (show.audience_rating.has_value()) s.bind(15, show.audience_rating.value());
            else                                  s.bind(15);
            s.bind(16, show.labels);
            s.bind(17, show.network);
            s.bind(18, show.actors);
            s.bind(19, show.countries);
            s.bind(20, show.collections);
            s.exec();

            upsertMapping("show", kairos_id, source_id, library_id, ext_show_id);
        }
        txn.commit();
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
                    auto& eps = episodes_by_show[i] = src.fetchEpisodes(ext_show_ids[i]);
                    for (auto& ep : eps)
                        ep.duration_ms = validateDurationMs(ep.duration_ms, conf_.applyPathMap(ep.file_path));
                    std::lock_guard lock(s_log_mu);
                    std::cout << "[sync]     \"" << shows[i].title << "\": "
                              << eps.size() << " episode(s)" << std::endl;
                }
            });
        }
        for (auto& t : workers) t.join();
    }

    SQLite::Transaction txn(db_.get());
    for (size_t i = 0; i < shows.size(); ++i) {
        auto& show     = shows[i];
        auto& episodes = episodes_by_show[i];

        // Clear existing episodes before reinserting the current set.
        // media_cursor.episode_id references episode(episode_id) with no ON DELETE
        // clause (RESTRICT), so we must nullify those refs before deleting episodes.
        { SQLite::Statement q(db_.get(),
              "UPDATE media_cursor SET episode_id = NULL "
              "WHERE episode_id IN (SELECT episode_id FROM episode WHERE show_id = ?)");
          q.bind(1, show.show_id); q.exec(); }
        { SQLite::Statement d(db_.get(), "DELETE FROM episode WHERE show_id=?");
          d.bind(1, show.show_id); d.exec(); }

        for (auto& ep : episodes) {
            const std::string ext_ep_id = ep.episode_id;
            ep.episode_id = resolveId("episode", source_id, ext_ep_id);
            ep.show_id    = show.show_id;

            SQLite::Statement e(db_.get(), R"(
                INSERT INTO episode (episode_id, show_id, season, episode, title, file_path,
                                     duration_ms, overview, air_date, thumb, absolute_index)
                VALUES (?,?,?,?,?,?,?,?,?,?,?)
            )");
            e.bind(1,  ep.episode_id);
            e.bind(2,  ep.show_id);
            e.bind(3,  ep.season);
            e.bind(4,  ep.episode);
            e.bind(5,  ep.title);
            e.bind(6,  ep.file_path);
            e.bind(7,  ep.duration_ms);
            e.bind(8,  ep.overview);
            e.bind(9,  ep.air_date);
            e.bind(10, ep.thumb);
            if (ep.absolute_index.has_value()) e.bind(11, ep.absolute_index.value());
            else                               e.bind(11);
            e.exec();

            upsertMapping("episode", ep.episode_id, source_id, library_id, ext_ep_id);
        }
    }

    // Remove shows that were in this library last sync but not returned this time.
    // Collect stale IDs before deleting so we don't modify while iterating.
    std::vector<std::string> stale_shows;
    {
        SQLite::Statement q(db_.get(),
            "SELECT kairos_id FROM source_mapping "
            "WHERE item_type='show' AND source_id=? AND library_id=?");
        q.bind(1, source_id); q.bind(2, library_id);
        while (q.executeStep()) {
            const std::string kid = q.getColumn(0).getString();
            if (!live_show_ids.contains(kid)) stale_shows.push_back(kid);
        }
    }
    for (const auto& kid : stale_shows) {
        // episode.show_id has no ON DELETE CASCADE, so delete episodes explicitly.
        // Nullify media_cursor.episode_id refs first (FK: RESTRICT).
        { SQLite::Statement q(db_.get(),
              "UPDATE media_cursor SET episode_id = NULL "
              "WHERE episode_id IN (SELECT episode_id FROM episode WHERE show_id = ?)");
          q.bind(1, kid); q.exec(); }
        { SQLite::Statement d(db_.get(), "DELETE FROM episode WHERE show_id = ?");
          d.bind(1, kid); d.exec(); }
        { SQLite::Statement d(db_.get(), "DELETE FROM show WHERE show_id=?");
          d.bind(1, kid); d.exec(); }
        std::cout << "[sync]   removed stale show: " << kid << std::endl;
    }

    // Prune source_mapping for any removed shows or orphaned episode entries.
    { SQLite::Statement d(db_.get(),
          "DELETE FROM source_mapping WHERE item_type='show' AND source_id=? AND library_id=?"
          " AND kairos_id NOT IN (SELECT show_id FROM show)");
      d.bind(1, source_id); d.bind(2, library_id); d.exec(); }
    { SQLite::Statement d(db_.get(),
          "DELETE FROM source_mapping WHERE item_type='episode' AND source_id=? AND library_id=?"
          " AND kairos_id NOT IN (SELECT episode_id FROM episode)");
      d.bind(1, source_id); d.bind(2, library_id); d.exec(); }

    txn.commit();
}

// ---------------------------------------------------------------------------
// Movie sync
// ---------------------------------------------------------------------------

void SyncManager::syncMovies(MediaSource& src,
                              const std::string& source_id,
                              const std::string& library_id,
                              const std::string& external_lib_id) {
    auto movies = src.fetchMovies(external_lib_id);
    std::cout << "[sync]   " << external_lib_id << ": " << movies.size() << " movie(s)" << std::endl;
    if (movies.empty()) return;

    std::unordered_set<std::string> live_movie_ids;
    SQLite::Transaction txn(db_.get());

    for (auto& movie : movies) {
        const std::string ext_movie_id = movie.movie_id;
        movie.movie_id    = resolveId("movie", source_id, ext_movie_id);
        movie.duration_ms = validateDurationMs(movie.duration_ms, conf_.applyPathMap(movie.file_path));
        live_movie_ids.insert(movie.movie_id);

        SQLite::Statement s(db_.get(), R"(
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
        )");
        s.bind(1,  movie.movie_id);
        s.bind(2,  movie.title);
        s.bind(3,  movie.content_rating);
        s.bind(4,  movie.file_path);
        s.bind(5,  movie.duration_ms);
        if (movie.year.has_value())            s.bind(6,  movie.year.value());
        else                                   s.bind(6);
        s.bind(7,  movie.overview);
        s.bind(8,  movie.tagline);
        s.bind(9,  movie.studio);
        s.bind(10, movie.director);
        s.bind(11, movie.genres);
        s.bind(12, movie.thumb);
        s.bind(13, movie.art);
        s.bind(14, movie.imdb_id);
        s.bind(15, movie.tmdb_id);
        if (movie.audience_rating.has_value()) s.bind(16, movie.audience_rating.value());
        else                                   s.bind(16);
        s.bind(17, movie.labels);
        s.bind(18, movie.actors);
        s.bind(19, movie.countries);
        s.bind(20, movie.collections);
        s.exec();

        upsertMapping("movie", movie.movie_id, source_id, library_id, ext_movie_id);
    }

    std::vector<std::string> stale_movies;
    {
        SQLite::Statement q(db_.get(),
            "SELECT kairos_id FROM source_mapping "
            "WHERE item_type='movie' AND source_id=? AND library_id=?");
        q.bind(1, source_id); q.bind(2, library_id);
        while (q.executeStep()) {
            const std::string kid = q.getColumn(0).getString();
            if (!live_movie_ids.contains(kid)) stale_movies.push_back(kid);
        }
    }
    for (const auto& kid : stale_movies) {
        SQLite::Statement d(db_.get(), "DELETE FROM movie WHERE movie_id=?");
        d.bind(1, kid); d.exec();
        std::cout << "[sync]   removed stale movie: " << kid << std::endl;
    }

    { SQLite::Statement d(db_.get(),
          "DELETE FROM source_mapping WHERE item_type='movie' AND source_id=? AND library_id=?"
          " AND kairos_id NOT IN (SELECT movie_id FROM movie)");
      d.bind(1, source_id); d.bind(2, library_id); d.exec(); }

    txn.commit();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string SyncManager::resolveId(const std::string& item_type,
                                    const std::string& source_id,
                                    const std::string& external_id) const {
    SQLite::Statement q(db_.get(),
        "SELECT kairos_id FROM source_mapping "
        "WHERE item_type = ? AND source_id = ? AND external_id = ?");
    q.bind(1, item_type);
    q.bind(2, source_id);
    q.bind(3, external_id);
    if (q.executeStep())
        return q.getColumn(0).getString();
    return source_id + ":" + external_id; // deterministic, no UUID needed
}

void SyncManager::upsertMapping(const std::string& item_type,
                                 const std::string& kairos_id,
                                 const std::string& source_id,
                                 const std::string& library_id,
                                 const std::string& external_id) {
    SQLite::Statement s(db_.get(), R"(
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
        SQLite::Statement sq(db_.get(),
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
        SQLite::Statement q(db_.get(),
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
                    SQLite::Statement lk(db_.get(),
                        "SELECT kairos_id FROM source_mapping "
                        "WHERE source_id=? AND external_id=? AND item_type=?");
                    lk.bind(1, source_id); lk.bind(2, rk); lk.bind(3, it);
                    if (lk.executeStep()) items.push_back({ it, lk.getColumn(0).getString() });
                }
            }

            const std::string fk_col   = (link.list_type == "playlist") ? "playlist_id"    : "filler_list_id";
            const std::string item_tbl = (link.list_type == "playlist") ? "playlist_item"  : "filler_list_item";

            SQLite::Transaction txn(db_.get());
            SQLite::Statement del(db_.get(),
                "DELETE FROM " + item_tbl + " WHERE " + fk_col + " = ?");
            del.bind(1, link.list_id); del.exec();

            int pos = 0;
            for (const auto& item : items) {
                SQLite::Statement ins(db_.get(),
                    "INSERT OR IGNORE INTO " + item_tbl +
                    " (" + fk_col + ", position, item_type, item_id) VALUES (?,?,?,?)");
                ins.bind(1, link.list_id); ins.bind(2, pos++);
                ins.bind(3, item.item_type); ins.bind(4, item.kairos_id);
                ins.exec();
            }

            SQLite::Statement ts(db_.get(),
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

MediaSource* SyncManager::findSource(const std::string& source_id) const {
    for (const auto& s : sources_)
        if (s->sourceId() == source_id) return s.get();
    return nullptr;
}

std::unique_ptr<MediaSource> SyncManager::buildSource(const std::string& source_id,
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
