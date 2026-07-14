#include "SyncManager.h"
#include "EmbySource.h"
#include "JellyfinSource.h"
#include "LocalSource.h"
#include "MediaProbe.h"
#include "PlexSource.h"
#include "conf/ConfStore.h"
#include "db/ChapterRepository.h"
#include "db/Database.h"
#include "detect/ChapterDetectionManager.h"
#include "log/DebugLog.h"
#include "scraper/ScraperManager.h"
#include "util/PathMatch.h"
#include "util/TitleMatch.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

namespace {
// Guarantees media_locked_ clears on every exit path out of syncAll()/
// syncSource() — including an exception thrown mid-phase — so a failure
// partway through a sync can't leave every show/movie/chapter mutation
// endpoint permanently 423-locked until the process restarts.
struct MediaLockGuard {
    std::atomic<bool>& flag;
    explicit MediaLockGuard(std::atomic<bool>& f) : flag(f) { flag.store(true); }
    ~MediaLockGuard() { flag.store(false); }
};
} // namespace

SyncManager::SyncManager(Database& db, ConfStore& conf)
    : db_(db), conf_(conf), sync_db_(db.openConnection(60000)) {}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

namespace {
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
        const std::string sid   = q.getColumn(0).getString();
        const std::string stype = q.getColumn(1).getString();
        const std::string surl  = q.getColumn(2).getString();
        try {
            auto src = buildSource(sid, stype, surl);
            if (src) sources_.push_back(std::move(src));
        } catch (const std::exception& e) {
            std::cerr << "[sync] failed to load source '" << sid << "': " << e.what()
                      << " — skipping\n";
        }
    }
    std::cout << "[sync] loaded " << sources_.size() << " source(s)" << std::endl;
}

// ---------------------------------------------------------------------------
// Public sync interface
// ---------------------------------------------------------------------------

void SyncManager::triggerSync(const std::string& source_id, const std::string& library_id) {
    bool expected = false;
    if (!sync_running_.compare_exchange_strong(expected, true)) {
        std::cout << "[sync] already running — ignoring trigger" << std::endl;
        return;
    }
    std::thread([this, source_id, library_id]() {
        try {
            if (!library_id.empty())
                syncLibrary(source_id, library_id);
            else if (source_id.empty())
                syncAll();
            else
                syncSource(source_id);
        } catch (const std::exception& e) {
            std::cerr << "[sync] error: " << e.what() << std::endl;
        }
        { std::lock_guard<std::mutex> lock(current_source_mtx_); current_source_id_.clear(); }
        sync_running_.store(false);
    }).detach();
}

void SyncManager::triggerHardSync(const std::string& source_id) {
    bool expected = false;
    if (!sync_running_.compare_exchange_strong(expected, true)) {
        std::cout << "[sync] already running — ignoring hard-sync trigger" << std::endl;
        return;
    }
    std::thread([this, source_id]() {
        try {
            clearSourceMapping(source_id);
            if (source_id.empty())
                syncAll();
            else
                syncSource(source_id);
        } catch (const std::exception& e) {
            std::cerr << "[sync] hard sync error: " << e.what() << std::endl;
        }
        { std::lock_guard<std::mutex> lock(current_source_mtx_); current_source_id_.clear(); }
        sync_running_.store(false);
    }).detach();
}

// Wipes source_mapping (this source only, or every row when source_id is
// empty) so the next syncContent() pass can't take the "already known" fast
// path for any affected item — path/title dedup and fresh-id assignment run
// exactly as they would the first time that source was ever synced. Note
// this also discards any manual cross-source links ("Link Existing")
// involving the affected item(s); that's expected, since a first-ever sync
// couldn't have had any either.
void SyncManager::clearSourceMapping(const std::string& source_id) {
    std::cout << "[sync] hard sync: clearing existing mappings"
              << (source_id.empty() ? " (all sources)" : " for " + source_id) << std::endl;
    if (source_id.empty()) {
        SQLite::Statement d(db_.get(), "DELETE FROM source_mapping");
        d.exec();
    } else {
        SQLite::Statement d(db_.get(), "DELETE FROM source_mapping WHERE source_id = ?");
        d.bind(1, source_id);
        d.exec();
    }
}

void SyncManager::syncAll() {
    sync_db_ = db_.openConnection(60000);
    MediaLockGuard media_lock(media_locked_);
    const auto t_total = std::chrono::steady_clock::now();

    // Phase 1: ingest content from every source.
    // All DB reads happen up front per library (snapshot); fetch and write
    // phases do not interleave reads and writes on sync_db_.
    DLOG << "[sync] === phase 1: content ingestion ===\n";
    SyncLiveIds live;
    for (const auto& src : sources_) {
        if (!src->isSupported()) {
            std::cout << "[sync] " << src->sourceId()
                      << " (" << src->sourceType() << ") not yet supported" << std::endl;
            continue;
        }
        syncContent(src->sourceId(), live);
    }
    { std::lock_guard<std::mutex> lock(current_source_mtx_); current_source_id_.clear(); }

    // Phase 1b: orphan cleanup — runs after ALL sources are known so a show
    // present in source B is never deleted because source A dropped it.
    DLOG << "[sync] === phase 1b: orphan cleanup ===\n";
    runOrphanCleanup(live);

    // Phase 2: scraper matching — blocking so chapters don't race against it.
    DLOG << "[sync] === phase 2: scraper match ===\n";
    if (scraper_) scraper_->runMatchSync();

    // Phase 2b: specials scan — opt-in per show, only for shows already matched.
    DLOG << "[sync] === phase 2b: specials scan ===\n";
    scanSpecialsForEligibleShows();

    // Phase 3: chapter sync.
    DLOG << "[sync] === phase 3: chapter sync ===\n";
    for (const auto& src : sources_) {
        if (src->isSupported())
            syncChaptersFromFiles(src->sourceId());
    }

    std::cout << "[sync] all sources done (total "
              << elapsedMs(t_total, std::chrono::steady_clock::now()) << "ms)" << std::endl;
}

void SyncManager::syncContent(const std::string& source_id, SyncLiveIds& live,
                              const std::string& library_id) {
    IMediaSource* src = findSource(source_id);
    if (!src || !src->isSupported()) return;

    {
        std::lock_guard<std::mutex> lock(current_source_mtx_);
        current_source_id_ = source_id;
    }

    // Touch all three per-source sets unconditionally, even if this source
    // ends up with zero show/movie libraries this round (e.g. its only
    // library's type just flipped to "movie"). Without this, syncShows()/
    // syncMovies() never running at all leaves no key for this source_id in
    // by_source_shows/by_source_movies, so runOrphanCleanup's pruneMapping()
    // can't tell "this source reported zero shows" from "this source was
    // never considered" — it silently skips pruning and old mappings (plus
    // their show/episode rows) linger forever instead of self-healing.
    live.by_source_shows[source_id];
    live.by_source_episodes[source_id];
    live.by_source_movies[source_id];

    std::cout << "[sync] content: " << source_id
              << (library_id.empty() ? "" : " / library " + library_id) << std::endl;

    // Drain the cursor before calling syncShows/syncMovies so no read cursor
    // on sync_db_ is live when BEGIN IMMEDIATE transactions start.
    std::string source_display;
    {
        SQLite::Statement q(sync_db_,
            "SELECT display_name FROM media_source WHERE source_id = ?");
        q.bind(1, source_id);
        source_display = q.executeStep() ? q.getColumn(0).getString() : source_id;
    }

    struct LibRow { std::string library_id, external_lib_id, library_type, display_name; };
    std::vector<LibRow> libs;
    {
        SQLite::Statement q(sync_db_, library_id.empty()
            ? "SELECT ml.library_id, ml.external_lib_id, ml.library_type, ml.display_name, ms.sync_priority "
              "FROM media_library ml "
              "JOIN media_source ms ON ms.source_id = ml.source_id "
              "WHERE ml.source_id = ? AND ml.enabled = 1 "
              "ORDER BY ms.sync_priority"
            : "SELECT ml.library_id, ml.external_lib_id, ml.library_type, ml.display_name, ms.sync_priority "
			  "FROM media_library ml "
			  "JOIN media_source ms ON ms.source_id = ml.source_id "
			  "WHERE ml.source_id = ? AND ml.enabled = 1 AND ml.library_id = ? "
			  "ORDER BY ms.sync_priority");
        q.bind(1, source_id);
        if (!library_id.empty()) q.bind(2, library_id);
        while (q.executeStep()) {
            libs.push_back({
                q.getColumn(0).getString(),
                q.getColumn(1).getString(),
                q.getColumn(2).getString(),
                q.getColumn(3).getString()
            });
        }
    }

    // User discovery — every account/profile the source reports, so admins can
    // see who exists on their servers that Pantheon doesn't have a local
    // account for yet. One cheap extra request per source per sync. Raw SQL
    // against sync_db_ rather than SourceRepository (which wraps the primary
    // connection) — same reasoning as every other write in this file: this
    // runs on syncSource's own connection, not the one repositories use.
    // Jellyfin/Emby's own configured primary account (the identity syncing
    // runs as) is excluded so it's never offered as "unregistered" itself;
    // Plex has no such concept (identity is implicit in the token) so nothing
    // is excluded there — best-effort, not a guarantee, per
    // IMediaSource::listServerUsers's doc comment.
    {
        auto users = src->listServerUsers();
        const std::string primary_user_id = conf_.userId(source_id);
        if (!primary_user_id.empty()) {
            users.erase(std::remove_if(users.begin(), users.end(),
                [&](const SourceUserInfo& u) { return u.external_user_id == primary_user_id; }),
                users.end());
        }
        if (!users.empty()) {
            SQLite::Statement s(sync_db_, R"(
                INSERT INTO source_user (source_id, external_user_id, display_name, email, last_seen_at)
                VALUES (?, ?, ?, ?, ?)
                ON CONFLICT(source_id, external_user_id) DO UPDATE SET
                    display_name = excluded.display_name,
                    email        = excluded.email,
                    last_seen_at = excluded.last_seen_at
            )");
            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            for (const auto& u : users) {
                s.bind(1, source_id);
                s.bind(2, u.external_user_id);
                s.bind(3, u.display_name);
                s.bind(4, u.email);
                s.bind(5, now);
                s.exec();
                s.reset();
            }
        }
    	
    	DLOG << "[sync] user discovery: " << source_id << " / " << users.size() << " users" << std::endl;

        // See IMediaSource::lastUserDiscoveryError() — surfaces a real
        // permission/auth failure (vs. "this server genuinely has no other
        // users") in the UI instead of only a stderr log line.
        SQLite::Statement us(sync_db_,
            "UPDATE media_source SET user_sync_error = ?, user_sync_checked_at = ? WHERE source_id = ?");
        us.bind(1, src->lastUserDiscoveryError());
        us.bind(2, static_cast<int64_t>(std::time(nullptr)));
        us.bind(3, source_id);
        us.exec();
    }

	if (true)
	{
		for (const auto& lib : libs) {
			const std::string label = source_display + " / " + lib.display_name;
			if (lib.library_type == "show" || lib.library_type == "mixed")
				syncShows(*src, source_id, lib.library_id, lib.external_lib_id, lib.library_type, label, live);
			if (lib.library_type == "movie" || lib.library_type == "mixed")
				syncMovies(*src, source_id, lib.library_id, lib.external_lib_id, lib.library_type, label, live);
		}
	}
	
    // Needs this source's source_mapping freshly populated above to resolve
    // external ids, so it runs after the library loop, not interleaved with it.
    syncLinkedUserWatchState(*src, source_id);

    syncPlexLinks(source_id);
}

void SyncManager::syncSource(const std::string& source_id) {
    sync_db_ = db_.openConnection(60000);
    MediaLockGuard media_lock(media_locked_);
    SyncLiveIds live;
    syncContent(source_id, live);
    runOrphanCleanup(live);
    if (scraper_) scraper_->runMatchSync();
    scanSpecialsForEligibleShows();
    syncChaptersFromFiles(source_id);
    std::cout << "[sync] done: " << source_id << std::endl;
}

void SyncManager::syncLibrary(const std::string& source_id, const std::string& library_id) {
    sync_db_ = db_.openConnection(60000);
    MediaLockGuard media_lock(media_locked_);
    SyncLiveIds live; // scoped to this one library — not a valid input to runOrphanCleanup, see header comment
    syncContent(source_id, live, library_id);
    if (scraper_) scraper_->runMatchSync();
    std::cout << "[sync] done: " << source_id << " / library " << library_id << std::endl;
}

// ---------------------------------------------------------------------------
// Show + episode sync
// ---------------------------------------------------------------------------

namespace {
constexpr int    kEpisodeFetchConcurrency = 8;
constexpr size_t kShowMetaBatchSize       = 100;
constexpr size_t kMovieBatchSize          = 200;
constexpr size_t kEpisodeBatchSize        = 50; // shows per write transaction

// Deterministic composite key for duplicate_candidate — the pair is sorted
// so the same physical pair always collides to the same row regardless of
// which side was "new" vs "existing" in a given sync run. This is also the
// entire remembered-dismissal mechanism: paired with an
// "ON CONFLICT(candidate_id) DO NOTHING" insert, a dismissed (or already
// pending) row is never touched again by a later sync re-detecting it.
std::string dupCandidateKey(const std::string& item_type,
                             const std::string& id_a, const std::string& id_b) {
    return item_type + ":" + (id_a < id_b ? id_a + ":" + id_b : id_b + ":" + id_a);
}

std::string dupCandidateReason(const std::string& trigger, double title_similarity) {
    const int pct = static_cast<int>(title_similarity * 100 + 0.5);
    if (trigger == "folder_uncertain") return "Same folder, title similarity " + std::to_string(pct) + "%";
    if (trigger == "both")             return "Same folder (fuzzy match), title similarity " + std::to_string(pct) + "%";
    return "Title similarity " + std::to_string(pct) + "%, different folders";
}

// Shared watch_progress freshness-merge policy — used for the primary
// synced_user_id path (syncShows()/syncMovies() below) and for every other
// linked account (syncLinkedUserWatchState()). A source write only wins when
// it's provably fresher (src_watched_at newer than the local row's
// updated_at) or there's no local row yet at all — seeds Continue Watching
// from the source once, but never clobbers progress the user just made in
// Hades itself. A source-reported "fully watched" with no local row is
// skipped entirely (not inserted at 100%), matching watch_progress's
// existing convention that a missing row means "not in progress"
// (PlaybackService deletes on >=95% watched).
void applyWatchState(SQLite::Statement& s_get, SQLite::Statement& s_upsert, SQLite::Statement& s_delete,
                      const std::string& user_id, const std::string& item_type, const std::string& content_id,
                      std::optional<bool> src_watched, std::optional<int64_t> src_position_ms,
                      std::optional<int64_t> src_watched_at, int64_t duration_ms) {
    if (user_id.empty() || (!src_watched.has_value() && !src_position_ms.has_value())) return;

    s_get.reset();
    s_get.bind(1, user_id);
    s_get.bind(2, item_type);
    s_get.bind(3, content_id);
    const bool has_local = s_get.executeStep();
    const int64_t local_updated_at = has_local ? s_get.getColumn(0).getInt64() : 0;

    const bool source_fresher = src_watched_at.has_value()
        ? (src_watched_at.value() > local_updated_at)
        : !has_local; // no timestamp from source: seed once, never overwrite
    if (!source_fresher) return;

    if (src_watched.value_or(false)) {
        if (has_local) {
            s_delete.reset();
            s_delete.bind(1, user_id);
            s_delete.bind(2, item_type);
            s_delete.bind(3, content_id);
            s_delete.exec();
        }
    } else if (src_position_ms.has_value()) {
        s_upsert.reset();
        s_upsert.bind(1, user_id);
        s_upsert.bind(2, item_type);
        s_upsert.bind(3, content_id);
        s_upsert.bind(4, src_position_ms.value());
        s_upsert.bind(5, duration_ms);
        s_upsert.bind(6, src_watched_at.value_or(static_cast<int64_t>(std::time(nullptr))));
        s_upsert.exec();
    }
}

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
                             const std::string& external_lib_id,
                             const std::string& library_type,
                             const std::string& label,
                             SyncLiveIds& live) {
    // ── Snapshot load ────────────────────────────────────────────────────────
    // All DB reads happen here before any fetch or write.  The write phase
    // uses in-memory maps for ID resolution — no reads inside transactions.

    const std::string show_prefix = source_id + ":";
    const std::string ep_prefix   = source_id + ":";

    // source_mapping for shows in this library: ext_id → kairos_id
    std::unordered_map<std::string, std::string> show_ext_to_kairos;
    {
        SQLite::Statement q(sync_db_,
            "SELECT external_id, kairos_id FROM source_mapping "
            "WHERE item_type='show' AND source_id=? AND library_id=?");
        q.bind(1, source_id); q.bind(2, library_id);
        while (q.executeStep())
            show_ext_to_kairos[q.getColumn(0).getString()] = q.getColumn(1).getString();
    }

    // Cross-source dedup: lowercase title + year — title alone collides on
    // real shows (UK/US "The Office"/"Shameless", reboots sharing a name,
    // etc.), so this only merges when both sides agree on year or neither
    // has one. See the matching movie-dedup key below for the same reasoning.
	std::unordered_map<std::string, std::string> show_title_to_id;
	struct ShowSnapshot { std::string kairos_id, title, folder_path; };
	std::unordered_map<std::string, ShowSnapshot> folder_exact_to_show;
	std::unordered_map<std::string, ShowSnapshot> folder_ci_to_show;
	std::unordered_map<std::string, std::vector<ShowSnapshot>> shows_by_year;
    {
        SQLite::Statement q(sync_db_, "SELECT LOWER(title), year, show_id, folder_path FROM show");
        while (q.executeStep()) {
            std::string key = q.getColumn(0).getString() + "|" +
                (q.getColumn(1).isNull() ? "" : std::to_string(q.getColumn(1).getInt()));
            show_title_to_id[key] = q.getColumn(2).getString();
        	
        	ShowSnapshot snap{ q.getColumn(2).getString(), q.getColumn(0).getString(),
								q.getColumn(3).getString() };
        	std::string year_key = q.getColumn(1).isNull() ? "" : std::to_string(q.getColumn(1).getInt());
        	shows_by_year[year_key].push_back(snap);

        	if (!snap.folder_path.empty()) {
        		std::string mapped = conf_.applyPathMap(snap.folder_path);
        		folder_exact_to_show[pathutil::normalizeCheap(mapped)] = snap;
        		folder_ci_to_show[pathutil::normalizeCaseInsensitive(mapped)] = snap;
        	}
        }
    }

    // Sync-time dedup thresholds — see ScraperSettings::dedup_fuzzy_title_threshold
    // doc comment; proposed defaults expecting real-world tuning post-rollout.
    const ScraperSettings dedup_settings = scraper_ ? scraper_->getSettings() : ScraperSettings{};
    const double kFuzzyTitleThreshold          = dedup_settings.dedup_fuzzy_title_threshold;
    const double kFolderCorroborationThreshold = dedup_settings.dedup_folder_corroboration_threshold;

    // Holds an "uncertain duplicate" note for shows/index i that got a fresh
    // kairos_id minted below despite a fuzzy folder/title signal pointing at
    // an existing row — written to duplicate_candidate in the batch-write
    // loop further down, for human review rather than auto-merging.
    struct PendingDup {
        std::string other_kairos_id, other_title, other_folder;
        std::string trigger; // "fuzzy_title" | "folder_uncertain" | "both"
        double      title_similarity = 0;
    };

    // source_mapping for episodes from this source: ext_ep_id → kairos_ep_id
    std::unordered_map<std::string, std::string> ep_ext_to_kairos;
    {
        SQLite::Statement q(sync_db_,
            "SELECT external_id, kairos_id FROM source_mapping "
            "WHERE item_type='episode' AND source_id=?");
        q.bind(1, source_id);
        while (q.executeStep())
            ep_ext_to_kairos[q.getColumn(0).getString()] = q.getColumn(1).getString();
    }

    // Cross-source episode dedup: mapped(file_path) → kairos_ep_id
    std::unordered_map<std::string, std::string> ep_path_to_id;
    {
        SQLite::Statement q(sync_db_,
            "SELECT file_path, episode_id FROM episode WHERE file_path != ''");
        while (q.executeStep()) {
            std::string path = q.getColumn(0).getString();
            ep_path_to_id[conf_.applyPathMap(path)] = q.getColumn(1).getString();
        }
    }

    // ── Fetch shows ──────────────────────────────────────────────────────────
    std::cout << "[sync]   fetching shows: " << label << std::endl;
    const auto t_shows = std::chrono::steady_clock::now();
    auto shows = src.fetchShows(external_lib_id);
    std::cout << "[sync]   " << label << ": " << shows.size()
              << " show(s) (" << elapsedMs(t_shows, std::chrono::steady_clock::now()) << "ms)"
              << std::endl;
    if (shows.empty()) return;

    // ── ID resolution in memory ──────────────────────────────────────────────
    std::vector<std::string> ext_show_ids(shows.size());
    std::vector<bool>        cross_ref_shows(shows.size(), false);
    std::vector<std::optional<PendingDup>> pending_dup(shows.size());

    for (size_t i = 0; i < shows.size(); ++i) {
        auto& show = shows[i];
        const std::string ext_id = show.show_id;
        std::string kairos_id;
        bool is_cross_ref = false;

        auto it = show_ext_to_kairos.find(ext_id);
        if (it != show_ext_to_kairos.end()) {
            kairos_id = it->second;
            if (!kairos_id.starts_with(show_prefix)) is_cross_ref = true;
        } else {
            // Tier 1a — exact folder match (path-mapped + cheaply normalized),
            // corroborated by at least a loose title similarity. Tried before
            // the exact title+year check below: folder identity is the more
            // specific "same physical file" signal. An exact folder match
            // that ISN'T corroborated (e.g. a coincidental shared/anthology
            // folder) doesn't resolve here — it falls through as an
            // "uncertain" note instead of auto-merging.
            std::optional<PendingDup> dup_note;
            if (!show.folder_path.empty()) {
                std::string mapped = pathutil::normalizeCheap(conf_.applyPathMap(show.folder_path));
                auto fit = folder_exact_to_show.find(mapped);
                if (fit != folder_exact_to_show.end()) {
                	// Same filepath with wildly differing titles likely means either libraries are using differing scrapers or
                	// a bad match from one of the sources.
                	DLOG << "[folder match: " << fit->first << "]" <<'\n';
                    double sim = titlematch::titleSimilarity(show.title, fit->second.title);
                	DLOG << "[title similarity: " << show.title << " & " << fit->second.title << " : " << sim << "]" <<'\n';
                    if (sim >= kFolderCorroborationThreshold) {
                        kairos_id = fit->second.kairos_id;
                    } else {
                        dup_note = PendingDup{ fit->second.kairos_id, fit->second.title,
                                                fit->second.folder_path, "folder_uncertain", sim };
                    }
                }
            }

            // Tier 1b — existing exact title+year match, unchanged behavior.
            // A clean exact match wins outright over any pending folder note.
            if (kairos_id.empty()) {
                std::string lower = show.title;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                std::string key = lower + "|" +
                    (show.year.has_value() ? std::to_string(show.year.value()) : "");
                auto tit = show_title_to_id.find(key);
                if (tit != show_title_to_id.end()) {
                    kairos_id = tit->second;
                    dup_note.reset();
                }
            }

            // Tier 2b — folder match only via the case-insensitive fallback
            // (no exact hit at all in Tier 1a).
            if (kairos_id.empty() && !dup_note && !show.folder_path.empty()) {
                std::string ci = pathutil::normalizeCaseInsensitive(conf_.applyPathMap(show.folder_path));
                auto fit = folder_ci_to_show.find(ci);
                if (fit != folder_ci_to_show.end()) {
                    double sim = titlematch::titleSimilarity(show.title, fit->second.title);
                    dup_note = PendingDup{ fit->second.kairos_id, fit->second.title,
                                            fit->second.folder_path, "folder_uncertain", sim };
                }
            }

            // Tier 2a — fuzzy title (Levenshtein similarity + ±1 year
            // tolerance), independent of any folder signal. Merges into an
            // existing pending folder note if they point at the same
            // candidate; otherwise takes priority as the more specific
            // signal (a rare double-collision is simplified to one pair).
            if (kairos_id.empty()) {
                std::optional<PendingDup> title_note;
                double best_sim = 0;
                auto probeYear = [&](const std::string& year_key) {
                    auto yit = shows_by_year.find(year_key);
                    if (yit == shows_by_year.end()) return;
                    for (const auto& cand : yit->second) {
                        double sim = titlematch::titleSimilarity(show.title, cand.title);
                        if (sim >= kFuzzyTitleThreshold && sim > best_sim) {
                            best_sim = sim;
                            title_note = PendingDup{ cand.kairos_id, cand.title, cand.folder_path,
                                                      "fuzzy_title", sim };
                        }
                    }
                };
                if (show.year.has_value()) {
                    probeYear(std::to_string(show.year.value() - 1));
                    probeYear(std::to_string(show.year.value()));
                    probeYear(std::to_string(show.year.value() + 1));
                } else {
                    probeYear("");
                }

                if (title_note) {
                    if (dup_note && dup_note->other_kairos_id == title_note->other_kairos_id) {
                        dup_note->trigger = "both";
                        dup_note->title_similarity = std::max(dup_note->title_similarity, title_note->title_similarity);
                    } else {
                        dup_note = title_note;
                    }
                }
            }

            // Tier 3 — no match: mint a fresh id, same as always. Any pending
            // uncertain-duplicate note gets recorded against it below.
            if (kairos_id.empty()) {
                kairos_id = show_prefix + ext_id;
                pending_dup[i] = dup_note;
            }

            // Only a genuine cross-source dedup when the match belongs to
            // someone else's prefix. Any resolution above landing back on
            // this source's own deterministic id (e.g. after a hard sync
            // clears source_mapping and this show's row is rediscovered by
            // folder/title) is this source re-finding itself, not a merge —
            // treating it as cross-ref would wrongly skip the metadata
            // upsert below.
            if (!kairos_id.starts_with(show_prefix))
            {
            	DLOG << "[Show " << show.folder_path << + " - " << show.title 
            	<< " matched to: " << (dup_note ? dup_note->other_folder : " ") + " - "
            	<< (dup_note ? dup_note->other_title : " ") << "]" << '\n';
	            is_cross_ref = true;
            }
        	else
        	{
        		DLOG << "[new show registered: " << conf_.applyPathMap(show.folder_path) << "] "
        		<< (dup_note ? "Dupe Possible: " + dup_note->other_folder + " - " + dup_note->other_title + " triggered by: " + dup_note->trigger +  " with similarity: " + std::to_string(dup_note->title_similarity) : "") << '\n';
        	}
        }

        ext_show_ids[i]    = ext_id;
        cross_ref_shows[i] = is_cross_ref;
        show.show_id       = kairos_id;

        live.shows.insert(kairos_id);
        live.by_source_shows[source_id].insert(kairos_id);
    }

    // ── Write show metadata (Pass 2) ─────────────────────────────────────────
    SQLite::Statement s_upsert_show(sync_db_, R"(
        INSERT INTO show (show_id, title, content_rating, overview, studio, status,
                          genres, thumb, art, imdb_id, tvdb_id, tmdb_id,
                          originally_available_at, year, audience_rating,
                          labels, network, actors, countries, collections, folder_path)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
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
            collections             = CASE WHEN locked THEN collections             ELSE excluded.collections             END,
            folder_path             = CASE WHEN locked THEN folder_path             ELSE excluded.folder_path             END
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
            collections             != excluded.collections             OR
            folder_path             != excluded.folder_path
        )
    )");
    SQLite::Statement s_show_mapping(sync_db_, R"(
        INSERT INTO source_mapping (item_type, kairos_id, source_id, library_id, external_id)
        VALUES ('show',?,?,?,?)
        ON CONFLICT(item_type, kairos_id, source_id) DO UPDATE SET
            library_id  = excluded.library_id,
            external_id = excluded.external_id
    )");
    SQLite::Statement s_dup_candidate(sync_db_, R"(
        INSERT INTO duplicate_candidate
            (candidate_id, item_type, kairos_id_a, kairos_id_b, trigger, reason, title_similarity, folder_a, folder_b)
        VALUES (?,?,?,?,?,?,?,?,?)
        ON CONFLICT(candidate_id) DO NOTHING
    )");

    for (size_t batch_start = 0; batch_start < shows.size(); batch_start += kShowMetaBatchSize) {
        const size_t batch_end = std::min(batch_start + kShowMetaBatchSize, shows.size());
        yieldIfRequested();
        try {
            SQLite::Transaction txn(sync_db_, SQLite::TransactionBehavior::IMMEDIATE);
            for (size_t i = batch_start; i < batch_end; ++i) {
                const auto& show = shows[i];
                if (!cross_ref_shows[i]) {
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
                    s_upsert_show.bind(21, show.folder_path);
                    s_upsert_show.exec();
                }
                s_show_mapping.reset();
                s_show_mapping.bind(1, show.show_id);
                s_show_mapping.bind(2, source_id);
                s_show_mapping.bind(3, library_id);
                s_show_mapping.bind(4, ext_show_ids[i]);
                s_show_mapping.exec();

                if (pending_dup[i]) {
                    const auto& dup = *pending_dup[i];
                    const bool this_is_a = show.show_id < dup.other_kairos_id;
                    s_dup_candidate.reset();
                    s_dup_candidate.bind(1, dupCandidateKey("show", show.show_id, dup.other_kairos_id));
                    s_dup_candidate.bind(2, "show");
                    s_dup_candidate.bind(3, this_is_a ? show.show_id : dup.other_kairos_id);
                    s_dup_candidate.bind(4, this_is_a ? dup.other_kairos_id : show.show_id);
                    s_dup_candidate.bind(5, dup.trigger);
                    s_dup_candidate.bind(6, dupCandidateReason(dup.trigger, dup.title_similarity));
                    s_dup_candidate.bind(7, dup.title_similarity);
                    s_dup_candidate.bind(8, this_is_a ? show.folder_path : dup.other_folder);
                    s_dup_candidate.bind(9, this_is_a ? dup.other_folder : show.folder_path);
                    s_dup_candidate.exec();
                }
            }
            txn.commit();
            std::cout << "[sync]   wrote show metadata: "
                      << batch_end << "/" << shows.size() << std::endl;
        } catch (const SQLite::Exception& e) {
            std::cerr << "[sync] error writing show metadata batch "
                      << batch_start << "-" << batch_end
                      << ": " << e.what() << " (sqlite_errcode=" << e.getExtendedErrorCode() << ") — skipping\n";
        } catch (const std::exception& e) {
            std::cerr << "[sync] error writing show metadata batch "
                      << batch_start << "-" << batch_end
                      << ": " << e.what() << " — skipping\n";
        }
    }
    s_upsert_show.reset();
    s_show_mapping.reset();

    // ── Parallel episode fetch ────────────────────────────────────────────────
    std::cout << "[sync]   fetching episodes: " << shows.size() << " show(s)" << std::endl;
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
                  << total_eps << " episode(s) to db: " << label << std::endl;
    }
    const auto t_write = std::chrono::steady_clock::now();

    // In-memory episode ID resolution helpers — no DB reads during write phase.
    auto ep_resolve_id = [&](const std::string& ext) -> std::string {
        auto it = ep_ext_to_kairos.find(ext);
        return it != ep_ext_to_kairos.end() ? it->second : ep_prefix + ext;
    };
    auto ep_resolve_by_path = [&](const std::string& path) -> std::string {
        if (path.empty()) return "";
        std::string mapped = conf_.applyPathMap(path);
        auto it = ep_path_to_id.find(mapped);
        return it != ep_path_to_id.end() ? it->second : "";
    };

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
        WHERE (
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
        )
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

    // Watch-state seeding — see syncMovies()'s identical block for the
    // freshness/overwrite policy this follows.
    const std::string synced_user_id = [&] {
        SQLite::Statement q(sync_db_, "SELECT synced_user_id FROM media_source WHERE source_id = ?");
        q.bind(1, source_id);
        if (q.executeStep() && !q.getColumn(0).isNull()) return q.getColumn(0).getString();
        return std::string();
    }();
    SQLite::Statement s_watch_get(sync_db_,
        "SELECT updated_at FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");
    SQLite::Statement s_watch_upsert(sync_db_, R"(
        INSERT INTO watch_progress (user_id, content_type, content_id, position_ms, duration_ms, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(user_id, content_type, content_id) DO UPDATE SET
            position_ms = excluded.position_ms,
            duration_ms = excluded.duration_ms,
            updated_at  = excluded.updated_at
    )");
    SQLite::Statement s_watch_delete(sync_db_,
        "DELETE FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");

    for (size_t i = 0; i < shows.size(); ++i) {
        auto& show     = shows[i];
        auto& episodes = episodes_by_show[i];
        const bool cross_show = cross_ref_shows[i];

        yieldIfRequested();
        try {
            SQLite::Transaction txn(sync_db_, SQLite::TransactionBehavior::IMMEDIATE);

            std::unordered_map<int, std::string> season_names;
            for (auto& ep : episodes) {
                const std::string ext_ep_id = ep.episode_id;
                bool ep_cross_ref = false;
                std::string ep_kairos_id;

                // In both branches, a path/id match only counts as cross-ref
                // when it belongs to another source's prefix — a match that
                // resolves back to this source's own deterministic id (e.g.
                // rediscovering its own episode row by path after a hard
                // sync clears source_mapping) must still hit the upsert
                // below so its metadata actually gets refreshed.
                if (cross_show) {
                    ep_kairos_id = ep_resolve_by_path(ep.file_path);
                    if (!ep_kairos_id.empty()) {
                        if (!ep_kairos_id.starts_with(ep_prefix)) ep_cross_ref = true;
                    } else {
                        ep_kairos_id = ep_resolve_id(ext_ep_id);
                    }
                } else {
                    ep_kairos_id = ep_resolve_id(ext_ep_id);
                    if (ep_kairos_id == ep_prefix + ext_ep_id) {
                        const std::string existing = ep_resolve_by_path(ep.file_path);
                        if (!existing.empty()) {
                            ep_kairos_id = existing;
                            if (!existing.starts_with(ep_prefix)) ep_cross_ref = true;
                        }
                    } else if (!ep_kairos_id.starts_with(ep_prefix)) {
                        ep_cross_ref = true;
                    }
                }

                ep.episode_id = ep_kairos_id;
                ep.show_id    = show.show_id;

                live.episodes.insert(ep_kairos_id);
                live.by_source_episodes[source_id].insert(ep_kairos_id);

                try {
                    if (!ep_cross_ref) {
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

                    applyWatchState(s_watch_get, s_watch_upsert, s_watch_delete,
                                     synced_user_id, "episode", ep.episode_id,
                                     ep.src_watched, ep.src_position_ms, ep.src_watched_at, ep.duration_ms);
                } catch (const std::exception& e) {
                    std::cerr << "[sync] skipping episode " << ep.file_path
                              << ": " << e.what() << '\n';
                }

                if (!ep.season_name.empty() && !season_names.count(ep.season))
                    season_names[ep.season] = ep.season_name;
            }

            if (!cross_show) {
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
        } catch (const SQLite::Exception& e) {
            std::cerr << "[sync] error syncing show \"" << show.title
                      << "\": " << e.what()
                      << " (sqlite_errcode=" << e.getExtendedErrorCode() << ") — skipping\n";
        } catch (const std::exception& e) {
            std::cerr << "[sync] error syncing show \"" << show.title
                      << "\": " << e.what() << " — skipping\n";
        }
    }

    std::cout << "[sync]   episodes done: " << label
              << " (" << elapsedMs(t_write, std::chrono::steady_clock::now()) << "ms)" << std::endl;
}

// ---------------------------------------------------------------------------
// Movie sync
// ---------------------------------------------------------------------------

void SyncManager::syncMovies(IMediaSource& src,
                              const std::string& source_id,
                              const std::string& library_id,
                              const std::string& external_lib_id,
                              const std::string& library_type,
                              const std::string& label,
                              SyncLiveIds& live) {
    // ── Snapshot load ────────────────────────────────────────────────────────
    const std::string movie_prefix = source_id + ":";

    std::unordered_map<std::string, std::string> ext_to_kairos;
    {
        SQLite::Statement q(sync_db_,
            "SELECT external_id, kairos_id FROM source_mapping "
            "WHERE item_type='movie' AND source_id=? AND library_id=?");
        q.bind(1, source_id); q.bind(2, library_id);
        while (q.executeStep())
            ext_to_kairos[q.getColumn(0).getString()] = q.getColumn(1).getString();
    }

    std::unordered_map<std::string, std::string> path_to_kairos;
    {
        SQLite::Statement q(sync_db_,
            "SELECT file_path, movie_id FROM movie WHERE file_path != ''");
        while (q.executeStep()) {
            std::string path = q.getColumn(0).getString();
            path_to_kairos[conf_.applyPathMap(path)] = q.getColumn(1).getString();
        }
    }

    // Cross-source movie dedup: lowercase title + year. Title alone isn't
    // safe to merge on — remakes share a title across different years (e.g.
    // "Dune" 1984 vs 2021) — so this only fires when both sides agree on
    // year, or neither side has one; anything else falls through to a new
    // kairos entry (the Review queue's "Link Existing" flow is for exactly
    // this missed-dedup case — see the cross-source-merge notes).
    std::unordered_map<std::string, std::string> movie_title_to_id;
    {
        SQLite::Statement q(sync_db_, "SELECT LOWER(title), year, movie_id FROM movie");
        while (q.executeStep()) {
            std::string key = q.getColumn(0).getString() + "|" +
                (q.getColumn(1).isNull() ? "" : std::to_string(q.getColumn(1).getInt()));
            movie_title_to_id[key] = q.getColumn(2).getString();
        }
    }

    // Folder-path (parentDir of file_path) + year-bucket snapshots for the
    // tiered dedup below — see syncShows()'s identical structure for the
    // full reasoning. Tried in addition to, and after, the exact full-path
    // match above: file_path equality is even stronger evidence than folder
    // equality, so it keeps first claim; this catches the same-folder,
    // renamed-file case that an exact file_path compare can't.
    struct MovieSnapshot { std::string kairos_id, title, folder_path; };
    std::unordered_map<std::string, MovieSnapshot> folder_exact_to_movie;
    std::unordered_map<std::string, MovieSnapshot> folder_ci_to_movie;
    std::unordered_map<std::string, std::vector<MovieSnapshot>> movies_by_year;
    {
        SQLite::Statement q(sync_db_, "SELECT movie_id, title, year, file_path FROM movie WHERE file_path != ''");
        while (q.executeStep()) {
            std::string folder = pathutil::parentDir(q.getColumn(3).getString());
            MovieSnapshot snap{ q.getColumn(0).getString(), q.getColumn(1).getString(), folder };
            std::string year_key = q.getColumn(2).isNull() ? "" : std::to_string(q.getColumn(2).getInt());
            movies_by_year[year_key].push_back(snap);

            if (!folder.empty()) {
                std::string mapped = conf_.applyPathMap(folder);
                folder_exact_to_movie[pathutil::normalizeCheap(mapped)] = snap;
                folder_ci_to_movie[pathutil::normalizeCaseInsensitive(mapped)] = snap;
            }
        }
    }

    const ScraperSettings dedup_settings_m = scraper_ ? scraper_->getSettings() : ScraperSettings{};
    const double kFuzzyTitleThresholdM          = dedup_settings_m.dedup_fuzzy_title_threshold;
    const double kFolderCorroborationThresholdM = dedup_settings_m.dedup_folder_corroboration_threshold;

    struct PendingDup {
        std::string other_kairos_id, other_title, other_folder;
        std::string trigger; // "fuzzy_title" | "folder_uncertain" | "both"
        double      title_similarity = 0;
    };

    // ── Fetch ────────────────────────────────────────────────────────────────
    std::cout << "[sync]   fetching movies: " << label << std::endl;
    const auto t_fetch = std::chrono::steady_clock::now();
    auto movies = src.fetchMovies(external_lib_id);
    std::cout << "[sync]   " << label << ": " << movies.size()
              << " movie(s) (" << elapsedMs(t_fetch, std::chrono::steady_clock::now()) << "ms)"
              << std::endl;
    if (movies.empty()) return;
    std::vector<std::optional<PendingDup>> pending_dup(movies.size());

    // ── Parallel duration probe ───────────────────────────────────────────────
    {
        std::atomic<size_t> next{0};
        const int worker_count = std::min<int>(getThreadCount(),
                                                static_cast<int>(movies.size()));
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(worker_count));
        for (int w = 0; w < worker_count; ++w) {
            workers.emplace_back([&]() {
                for (size_t i = next.fetch_add(1); i < movies.size(); i = next.fetch_add(1)) {
                    const std::string mapped = conf_.applyPathMap(movies[i].file_path);
                    movies[i].duration_ms = validateDurationMs(movies[i].duration_ms, mapped);
                }
            });
        }
        for (auto& t : workers) t.join();
    }

    // ── ID resolution in memory ──────────────────────────────────────────────
    struct ResolvedMovie { std::string kairos_id, ext_id; bool is_cross_ref; };
    std::vector<ResolvedMovie> resolved(movies.size());

    for (size_t i = 0; i < movies.size(); ++i) {
        auto& movie = movies[i];
        const std::string ext_id = movie.movie_id;
        std::string kairos_id;
        bool is_cross_ref = false;

        auto it = ext_to_kairos.find(ext_id);
        if (it != ext_to_kairos.end()) {
            kairos_id = it->second;
            if (!kairos_id.starts_with(movie_prefix)) is_cross_ref = true;
        } else {
            std::optional<PendingDup> dup_note;

            // Exact full file_path match — stronger evidence than a
            // folder-only match (same file, not just same directory), so it
            // keeps first claim.
            std::string mapped = conf_.applyPathMap(movie.file_path);
            auto pit = path_to_kairos.find(mapped);
            if (pit != path_to_kairos.end()) {
                kairos_id = pit->second;
            }

            // Tier 1a — exact folder match (path-mapped + cheaply
            // normalized), corroborated by at least a loose title
            // similarity. See syncShows() for the full reasoning; an exact
            // folder match that ISN'T corroborated falls through as an
            // "uncertain" note instead of auto-merging.
            std::string folder = pathutil::parentDir(movie.file_path);
            if (kairos_id.empty() && !folder.empty()) {
                std::string fmapped = pathutil::normalizeCheap(conf_.applyPathMap(folder));
                auto fit = folder_exact_to_movie.find(fmapped);
                if (fit != folder_exact_to_movie.end()) {
                    double sim = titlematch::titleSimilarity(movie.title, fit->second.title);
                    if (sim >= kFolderCorroborationThresholdM) {
                        kairos_id = fit->second.kairos_id;
                    } else {
                        dup_note = PendingDup{ fit->second.kairos_id, fit->second.title,
                                                fit->second.folder_path, "folder_uncertain", sim };
                    }
                }
            }

            // Tier 1b — existing exact title+year match, unchanged behavior.
            if (kairos_id.empty()) {
                std::string lower = movie.title;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                std::string key = lower + "|" +
                    (movie.year.has_value() ? std::to_string(movie.year.value()) : "");
                auto tit = movie_title_to_id.find(key);
                if (tit != movie_title_to_id.end()) {
                    kairos_id = tit->second;
                    dup_note.reset();
                }
            }

            // Tier 2b — folder match only via the case-insensitive fallback.
            if (kairos_id.empty() && !dup_note && !folder.empty()) {
                std::string ci = pathutil::normalizeCaseInsensitive(conf_.applyPathMap(folder));
                auto fit = folder_ci_to_movie.find(ci);
                if (fit != folder_ci_to_movie.end()) {
                    double sim = titlematch::titleSimilarity(movie.title, fit->second.title);
                    dup_note = PendingDup{ fit->second.kairos_id, fit->second.title,
                                            fit->second.folder_path, "folder_uncertain", sim };
                }
            }

            // Tier 2a — fuzzy title (Levenshtein similarity + ±1 year
            // tolerance), independent of any folder signal.
            if (kairos_id.empty()) {
                std::optional<PendingDup> title_note;
                double best_sim = 0;
                auto probeYear = [&](const std::string& year_key) {
                    auto yit = movies_by_year.find(year_key);
                    if (yit == movies_by_year.end()) return;
                    for (const auto& cand : yit->second) {
                        double sim = titlematch::titleSimilarity(movie.title, cand.title);
                        if (sim >= kFuzzyTitleThresholdM && sim > best_sim) {
                            best_sim = sim;
                            title_note = PendingDup{ cand.kairos_id, cand.title, cand.folder_path,
                                                      "fuzzy_title", sim };
                        }
                    }
                };
                if (movie.year.has_value()) {
                    probeYear(std::to_string(movie.year.value() - 1));
                    probeYear(std::to_string(movie.year.value()));
                    probeYear(std::to_string(movie.year.value() + 1));
                } else {
                    probeYear("");
                }

                if (title_note) {
                    if (dup_note && dup_note->other_kairos_id == title_note->other_kairos_id) {
                        dup_note->trigger = "both";
                        dup_note->title_similarity = std::max(dup_note->title_similarity, title_note->title_similarity);
                    } else {
                        dup_note = title_note;
                    }
                }
            }

            if (kairos_id.empty()) {
                kairos_id = movie_prefix + ext_id;
                pending_dup[i] = dup_note;
            }

            // Same reasoning as the ext_id branch above: only cross-ref when
            // the match belongs to another source's prefix. A self-match
            // (this source rediscovering its own row, e.g. after a hard
            // sync clears source_mapping) must still go through the
            // metadata upsert below.
            if (!kairos_id.starts_with(movie_prefix)) is_cross_ref = true;
        }

        movie.movie_id = kairos_id;
        live.movies.insert(kairos_id);
        live.by_source_movies[source_id].insert(kairos_id);
        resolved[i] = {kairos_id, ext_id, is_cross_ref};
    }

    // ── Batch write (no DB reads) ─────────────────────────────────────────────
    const auto t_write = std::chrono::steady_clock::now();

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
    SQLite::Statement s_dup_candidate_m(sync_db_, R"(
        INSERT INTO duplicate_candidate
            (candidate_id, item_type, kairos_id_a, kairos_id_b, trigger, reason, title_similarity, folder_a, folder_b)
        VALUES (?,?,?,?,?,?,?,?,?)
        ON CONFLICT(candidate_id) DO NOTHING
    )");

    // Watch-state seeding — only when this source is opted in (media_source.
    // synced_user_id set). Policy applied per item below: a source write only
    // wins when it's provably fresher (src_watched_at newer than the local
    // row's updated_at) or there's no local row yet at all — this seeds
    // Continue Watching from the source once, but never clobbers progress the
    // user just made in Hades itself. A source-reported "fully watched" with
    // no local row is skipped entirely (not seeded) rather than inserted at
    // 100%, matching watch_progress's existing convention that a missing row
    // means "not in progress" (PlaybackService deletes on >=95% watched).
    const std::string synced_user_id = [&] {
        SQLite::Statement q(sync_db_, "SELECT synced_user_id FROM media_source WHERE source_id = ?");
        q.bind(1, source_id);
        if (q.executeStep() && !q.getColumn(0).isNull()) return q.getColumn(0).getString();
        return std::string();
    }();
    SQLite::Statement s_watch_get(sync_db_,
        "SELECT updated_at FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");
    SQLite::Statement s_watch_upsert(sync_db_, R"(
        INSERT INTO watch_progress (user_id, content_type, content_id, position_ms, duration_ms, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(user_id, content_type, content_id) DO UPDATE SET
            position_ms = excluded.position_ms,
            duration_ms = excluded.duration_ms,
            updated_at  = excluded.updated_at
    )");
    SQLite::Statement s_watch_delete(sync_db_,
        "DELETE FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");

    for (size_t batch_start = 0; batch_start < movies.size(); batch_start += kMovieBatchSize) {
        yieldIfRequested();
        const size_t batch_end = std::min(batch_start + kMovieBatchSize, movies.size());
        try {
            SQLite::Transaction txn(sync_db_, SQLite::TransactionBehavior::IMMEDIATE);
            for (size_t i = batch_start; i < batch_end; ++i) {
                const auto& movie = movies[i];
                const auto& res   = resolved[i];

                if (!res.is_cross_ref) {
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
                s_movie_mapping.bind(4, res.ext_id);
                s_movie_mapping.exec();

                if (pending_dup[i]) {
                    const auto& dup = *pending_dup[i];
                    const bool this_is_a = movie.movie_id < dup.other_kairos_id;
                    const std::string this_folder = pathutil::parentDir(movie.file_path);
                    s_dup_candidate_m.reset();
                    s_dup_candidate_m.bind(1, dupCandidateKey("movie", movie.movie_id, dup.other_kairos_id));
                    s_dup_candidate_m.bind(2, "movie");
                    s_dup_candidate_m.bind(3, this_is_a ? movie.movie_id : dup.other_kairos_id);
                    s_dup_candidate_m.bind(4, this_is_a ? dup.other_kairos_id : movie.movie_id);
                    s_dup_candidate_m.bind(5, dup.trigger);
                    s_dup_candidate_m.bind(6, dupCandidateReason(dup.trigger, dup.title_similarity));
                    s_dup_candidate_m.bind(7, dup.title_similarity);
                    s_dup_candidate_m.bind(8, this_is_a ? this_folder : dup.other_folder);
                    s_dup_candidate_m.bind(9, this_is_a ? dup.other_folder : this_folder);
                    s_dup_candidate_m.exec();
                }

                applyWatchState(s_watch_get, s_watch_upsert, s_watch_delete,
                                 synced_user_id, "movie", movie.movie_id,
                                 movie.src_watched, movie.src_position_ms, movie.src_watched_at, movie.duration_ms);
            }
            txn.commit();
            std::cout << "[sync]   wrote movies: "
                      << batch_end << "/" << movies.size() << std::endl;
        } catch (const SQLite::Exception& e) {
            std::cerr << "[sync] error writing movie batch "
                      << batch_start << "-" << batch_end
                      << ": " << e.what() << " (sqlite_errcode=" << e.getExtendedErrorCode() << ") — skipping\n";
        } catch (const std::exception& e) {
            std::cerr << "[sync] error writing movie batch "
                      << batch_start << "-" << batch_end
                      << ": " << e.what() << " — skipping\n";
        }
    }

    std::cout << "[sync]   movies done: " << label
              << " (" << elapsedMs(t_write, std::chrono::steady_clock::now()) << "ms)" << std::endl;
}

// ---------------------------------------------------------------------------
// Orphan cleanup — runs after all sources complete
// ---------------------------------------------------------------------------

void SyncManager::runOrphanCleanup(const SyncLiveIds& live) {
    std::cout << "[sync] orphan cleanup..." << std::endl;

    // Step 1: Remove stale source_mapping entries per source.
    // A stale entry is one the source didn't report this run.
    // We process each item type separately so we can batch per source.
    auto pruneMapping = [&](const std::string& item_type,
                             const std::unordered_map<std::string,
                                                       std::unordered_set<std::string>>& by_source) {
        for (const auto& [src_id, reported] : by_source) {
            std::vector<std::string> stale;
            {
                SQLite::Statement q(sync_db_,
                    "SELECT kairos_id FROM source_mapping WHERE item_type=? AND source_id=?");
                q.bind(1, item_type); q.bind(2, src_id);
                while (q.executeStep()) {
                    const std::string kid = q.getColumn(0).getString();
                    if (!reported.count(kid)) stale.push_back(kid);
                }
            }
            if (stale.empty()) continue;
            try {
                SQLite::Transaction txn(sync_db_, SQLite::TransactionBehavior::IMMEDIATE);
                for (const auto& kid : stale) {
                    SQLite::Statement d(sync_db_,
                        "DELETE FROM source_mapping "
                        "WHERE item_type=? AND source_id=? AND kairos_id=?");
                    d.bind(1, item_type); d.bind(2, src_id); d.bind(3, kid); d.exec();
                }
                txn.commit();
                std::cout << "[sync]   pruned " << stale.size() << " stale "
                          << item_type << " mapping(s) for source " << src_id << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[sync] error pruning " << item_type
                          << " mappings for " << src_id << ": " << e.what() << '\n';
            }
        }
    };

    pruneMapping("show",    live.by_source_shows);
    pruneMapping("episode", live.by_source_episodes);
    pruneMapping("movie",   live.by_source_movies);

    // Step 2: Delete media rows that have no remaining source_mapping entry.
    // Because we removed stale mappings above, a row with no mapping is
    // genuinely orphaned — no source claims it anymore.
    //
    // Each orphan is deleted in its own transaction, deliberately not one
    // bulk statement per type: an FK failure on one row (a stray reference
    // nothing here knows to clear) must only block that one row — every
    // other genuinely orphaned row still gets cleaned up — and gets logged
    // by name so the specific blocker is diagnosable instead of a generic
    // "orphan cleanup failed".
    //
    // show_season (ON DELETE CASCADE) and episode.linked_movie_id
    // (ON DELETE SET NULL) clean themselves up as a side effect of the show/
    // movie delete below — no separate statements needed for those.
    auto cleanupOrphans = [&](const std::string& item_type,
                               const std::string& table,
                               const std::string& id_col,
                               const std::string& extra_where,
                               const std::string& cursor_col) {
        std::vector<std::pair<std::string, std::string>> orphans;
        {
            SQLite::Statement q(sync_db_,
                "SELECT " + id_col + ", title FROM " + table +
                " WHERE " + id_col + " NOT IN "
                "(SELECT kairos_id FROM source_mapping WHERE item_type=?)" + extra_where);
            q.bind(1, item_type);
            while (q.executeStep()) {
                orphans.emplace_back(q.getColumn(0).getString(), q.getColumn(1).getString());
            }
        }
        int removed = 0;
        for (const auto& [id, title] : orphans) {
            try {
                SQLite::Transaction txn(sync_db_, SQLite::TransactionBehavior::IMMEDIATE);
                if (!cursor_col.empty()) {
                    // Nullify channel cursor refs before deleting (FK constraint).
                    SQLite::Statement nc(sync_db_,
                        "UPDATE media_cursor SET " + cursor_col + " = NULL WHERE " + cursor_col + " = ?");
                    nc.bind(1, id);
                    nc.exec();
                }
                if (item_type == "show" || item_type == "movie") {
                    // A pending "possible duplicate" candidate naming this
                    // now-orphaned id would otherwise dangle — plain TEXT
                    // kairos_id_a/b columns, no FK/cascade.
                    SQLite::Statement dc(sync_db_,
                        "DELETE FROM duplicate_candidate WHERE item_type=? AND (kairos_id_a=? OR kairos_id_b=?)");
                    dc.bind(1, item_type); dc.bind(2, id); dc.bind(3, id);
                    dc.exec();
                }
                SQLite::Statement d(sync_db_, "DELETE FROM " + table + " WHERE " + id_col + " = ?");
                d.bind(1, id);
                d.exec();
                txn.commit();
                ++removed;
            } catch (const std::exception& e) {
                std::cerr << "[sync] " << item_type << " '" << title << "' (" << id
                          << ") orphaned but could not be removed because " << e.what() << '\n';
            }
        }
        if (removed > 0) {
            std::cout << "[sync]   removed " << removed << " orphaned " << item_type << "(s)" << std::endl;
        }
    };

    // A linked special (episode.linked_movie_id set — see ScraperManager's
    // specials linking) has no underlying synced file, so it can never gain
    // a source_mapping row — excluded here so it isn't treated as orphaned.
    cleanupOrphans("episode", "episode", "episode_id",
                    " AND linked_movie_id IS NULL", "episode_id");
    cleanupOrphans("show", "show", "show_id", "", "");
    cleanupOrphans("movie", "movie", "movie_id", "", "movie_id");

    std::cout << "[sync] orphan cleanup complete" << std::endl;
}

void SyncManager::scanSpecialsForEligibleShows() {
    if (!scraper_) return;

    std::vector<std::string> show_ids;
    {
        SQLite::Statement q(sync_db_,
            "SELECT show_id FROM show WHERE find_specials = 1 AND match_status = 'matched'");
        while (q.executeStep()) show_ids.push_back(q.getColumn(0).getString());
    }
    if (show_ids.empty()) return;

    std::cout << "[sync] specials scan: " << show_ids.size() << " show(s) opted in" << std::endl;
    for (const auto& show_id : show_ids) {
        yieldIfRequested();
        try {
            scraper_->scanSpecialsForShow(show_id);
        } catch (const std::exception& e) {
            std::cerr << "[sync] specials scan error for " << show_id << ": " << e.what() << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::vector<std::string> SyncManager::sourceIds() const {
    std::vector<std::string> ids;
    ids.reserve(sources_.size());
    for (const auto& s : sources_) ids.push_back(s->sourceId());
    return ids;
}

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

void SyncManager::syncLinkedUserWatchState(IMediaSource& src, const std::string& source_id) {
    struct LinkedUser { std::string external_user_id, local_user_id; };
    std::vector<LinkedUser> linked;
    {
        SQLite::Statement q(sync_db_,
            "SELECT external_user_id, imported_user_id FROM source_user "
            "WHERE source_id = ? AND imported_user_id IS NOT NULL");
        q.bind(1, source_id);
        while (q.executeStep())
            linked.push_back({q.getColumn(0).getString(), q.getColumn(1).getString()});
    }
    if (linked.empty()) return;

    SQLite::Statement s_resolve(sync_db_,
        "SELECT kairos_id FROM source_mapping WHERE source_id = ? AND item_type = ? AND external_id = ?");
    SQLite::Statement s_duration_ep(sync_db_, "SELECT duration_ms FROM episode WHERE episode_id = ?");
    SQLite::Statement s_duration_mv(sync_db_, "SELECT duration_ms FROM movie WHERE movie_id = ?");
    SQLite::Statement s_watch_get(sync_db_,
        "SELECT updated_at FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");
    SQLite::Statement s_watch_upsert(sync_db_, R"(
        INSERT INTO watch_progress (user_id, content_type, content_id, position_ms, duration_ms, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(user_id, content_type, content_id) DO UPDATE SET
            position_ms = excluded.position_ms,
            duration_ms = excluded.duration_ms,
            updated_at  = excluded.updated_at
    )");
    SQLite::Statement s_watch_delete(sync_db_,
        "DELETE FROM watch_progress WHERE user_id=? AND content_type=? AND content_id=?");

    for (const auto& lu : linked) {
        // Empty for any source that doesn't override fetchWatchState (Plex
        // today) — nothing else in this loop runs for those.
        auto entries = src.fetchWatchState(lu.external_user_id);
        if (entries.empty()) continue;

        try {
            SQLite::Transaction txn(sync_db_, SQLite::TransactionBehavior::IMMEDIATE);
            for (const auto& e : entries) {
                s_resolve.reset();
                s_resolve.bind(1, source_id);
                s_resolve.bind(2, e.item_type);
                s_resolve.bind(3, e.external_id);
                if (!s_resolve.executeStep())
                {
                	DLOG << "[sync] linked user " << lu.external_user_id << " has no mapping for " << e.item_type << " " << e.external_id << '\n';
	                continue; // not in an enabled library
                }
                const std::string kairos_id = s_resolve.getColumn(0).getString();

                int64_t duration_ms = 0;
                SQLite::Statement& s_duration = (e.item_type == "movie") ? s_duration_mv : s_duration_ep;
                s_duration.reset();
                s_duration.bind(1, kairos_id);
                if (s_duration.executeStep()) duration_ms = s_duration.getColumn(0).getInt64();

            	DLOG << "[sync] syncing linked user watch state for " << e.title << " " << kairos_id << '\n';
                applyWatchState(s_watch_get, s_watch_upsert, s_watch_delete,
                                 lu.local_user_id, e.item_type, kairos_id,
                                 e.watched, e.position_ms, e.watched_at, duration_ms);
            }
            txn.commit();
        } catch (const std::exception& ex) {
            std::cerr << "[sync] linked-user watch state error (source=" << source_id
                      << " user=" << lu.external_user_id << "): " << ex.what() << '\n';
        }
    }
}

void SyncManager::syncPlexLinks(const std::string& source_id) {
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
    client.set_read_timeout(120); // see PlexSource's client_ setup for why 30s wasn't enough

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

            SQLite::Transaction txn(sync_db_, SQLite::TransactionBehavior::IMMEDIATE);
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
        return std::make_unique<LocalSource>(source_id, base_url, conf_);

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

    if (!external_id.empty()) {
        auto intro = src.fetchIntroMarkers(external_id);
        if (!intro.empty())
            repo.syncChapters(media_type, kairos_id, "plex_intro", std::move(intro));

        auto source_ch = src.fetchChapters(external_id);
        if (!source_ch.empty())
            repo.syncChapters(media_type, kairos_id, "plex_chapters", std::move(source_ch));
    }

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

    // ── Collect items to probe — close cursor before parallel workers start ──
    struct ProbeItem {
        std::string kairos_id;
        std::string media_type;
        std::string mapped_path;
        std::string show_id;    // episodes only — empty for movies
        std::string show_title; // for logging
    };
    std::vector<ProbeItem> items;

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
        while (q.executeStep()) {
            const std::string file_path = q.getColumn(1).getString();
            const int64_t     duration  = q.getColumn(4).getInt64();
            if (file_path.empty() || duration == 0) continue;
            const std::string mapped = conf_.applyPathMap(file_path);
            if (!std::filesystem::exists(mapped)) continue;
            items.push_back({
                q.getColumn(0).getString(),
                "episode",
                mapped,
                q.getColumn(2).getString(),
                q.getColumn(3).getString()
            });
        }
    }
    {
        SQLite::Statement q(sync_db_, R"(
            SELECT sm.kairos_id, m.file_path
            FROM source_mapping sm
            JOIN movie m ON m.movie_id = sm.kairos_id
            WHERE sm.item_type='movie' AND sm.source_id=?
        )");
        q.bind(1, source_id);
        while (q.executeStep()) {
            const std::string file_path = q.getColumn(1).getString();
            if (file_path.empty()) continue;
            const std::string mapped = conf_.applyPathMap(file_path);
            if (!std::filesystem::exists(mapped)) continue;
            items.push_back({q.getColumn(0).getString(), "movie", mapped, "", ""});
        }
    }

    if (items.empty()) {
        std::cout << "[sync] chapter sync done: " << source_id << " (nothing to probe)" << std::endl;
        return;
    }

    // ── Parallel ffprobe ─────────────────────────────────────────────────────
    struct ChapterResult {
        std::string kairos_id;
        std::string media_type;
        std::vector<Chapter> chapters;
    };
    std::vector<ChapterResult> results(items.size());

    {
        std::atomic<size_t> next{0};
        const int worker_count = std::min<int>(getThreadCount(),
                                                static_cast<int>(items.size()));
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(worker_count));
        for (int w = 0; w < worker_count; ++w) {
            workers.emplace_back([&]() {
                for (size_t i = next.fetch_add(1); i < items.size(); i = next.fetch_add(1)) {
                    auto ch = probeChapters(items[i].mapped_path);
                    if (!ch.empty())
                        results[i] = {items[i].kairos_id, items[i].media_type, std::move(ch)};
                }
            });
        }
        for (auto& t : workers) t.join();
    }

    // ── Write ────────────────────────────────────────────────────────────────
    ChapterRepository repo(db_);
    int written = 0;
    for (auto& res : results) {
        if (!res.chapters.empty()) {
            repo.syncChapters(res.media_type, res.kairos_id, "file", std::move(res.chapters));
            ++written;
        }
    }

    std::cout << "[sync] chapter sync done: " << source_id
              << " (" << written << " item(s) with chapters, "
              << elapsedMs(t_ch_start, std::chrono::steady_clock::now()) << "ms)" << std::endl;

    // Opportunistically kick off cross-episode intro/credits/recap/etc.
    // detection (ChapterDetector.h) for one show that doesn't have it yet.
    // Deliberately one show per sync pass, not every eligible one at once —
    // ChapterDetectionManager is single-flight and CPU/IO-heavy (full-file
    // audio fingerprinting per episode), so this trickles coverage across
    // the library over successive syncs instead of turning a routine sync
    // into a long detection marathon or silently dropping all but the first
    // trigger in a burst.
    if (chapter_detect_ && !chapter_detect_->isDetecting()) {
        std::unordered_set<std::string> seen;
        for (const auto& it : items) {
            if (it.show_id.empty() || seen.count(it.show_id)) continue;
            seen.insert(it.show_id);

            SQLite::Statement already(sync_db_, R"(
                SELECT 1 FROM chapter c JOIN episode e ON e.episode_id = c.media_id
                WHERE c.media_type='episode' AND e.show_id=? AND c.source='detected'
                  AND c.chapter_type IN ('intro','credits') LIMIT 1
            )");
            already.bind(1, it.show_id);
            if (already.executeStep()) continue; // already has detected anchors

            if (chapter_detect_->triggerShowDetect(it.show_id)) break; // one per sync pass
        }
    }
}
