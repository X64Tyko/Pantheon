#include "SyncManager.h"
#include "EmbySource.h"
#include "JellyfinSource.h"
#include "LocalSource.h"
#include "MediaProbe.h"
#include "PlexSource.h"
#include "conf/ConfStore.h"
#include "db/ChapterRepository.h"
#include "db/Database.h"
#include "db/DbHelpers.h"
#include "db/FilterExpr.h"
#include "db/MixedSort.h"
#include "db/SubtitleTrackRepository.h"
#include "detect/ChapterDetectionManager.h"
#include "log/DebugLog.h"
#include "scraper/ScraperManager.h"
#include "SubtitleSidecar.h"
#include "SubtitleValidation.h"
#include "util/PathMatch.h"
#include "util/TitleMatch.h"
#include "thread/TaskRegistry.h"
#include "metrics/OperationMetrics.h"
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
    // ORDER BY here is what makes syncAll()'s phase-1 loop (which just walks
    // sources_ directly) a priority-ordered sync, not just a priority-ordered
    // field-merge decision — see the show/movie upsert's primary_source logic
    // for the other half of this. source_id as a tiebreak keeps ordering
    // deterministic across reloads for sources tied on sync_priority (the
    // default, until a user actually ranks them).
    SQLite::Statement q(db_.get(),
        "SELECT source_id, source_type, COALESCE(base_url,'') "
        "FROM media_source WHERE enabled = 1 ORDER BY sync_priority ASC, source_id ASC");

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
    TaskRegistry::global().spawn([this, source_id, library_id]() {
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
    });
}

void SyncManager::triggerHardSync(const std::string& source_id) {
    bool expected = false;
    if (!sync_running_.compare_exchange_strong(expected, true)) {
        std::cout << "[sync] already running — ignoring hard-sync trigger" << std::endl;
        return;
    }
    TaskRegistry::global().spawn([this, source_id]() {
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
    });
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
    OperationRecorder full_rec("sync.full");
    const auto t_total = std::chrono::steady_clock::now();

    // Phase 1: ingest content from every source.
    // All DB reads happen up front per library (snapshot); fetch and write
    // phases do not interleave reads and writes on sync_db_.
    std::cout << "[sync] === phase 1: content ingestion ===\n";
    SyncLiveIds live;
    {
        OperationRecorder phase_rec("sync.phase.content");
        for (const auto& src : sources_) {
            if (!src->isSupported()) {
                std::cout << "[sync] " << src->sourceId()
                          << " (" << src->sourceType() << ") not yet supported" << std::endl;
                continue;
            }
            syncContent(src->sourceId(), live);
        }
    }
    { std::lock_guard<std::mutex> lock(current_source_mtx_); current_source_id_.clear(); }

    // Phase 1b: orphan cleanup — runs after ALL sources are known so a show
    // present in source B is never deleted because source A dropped it.
    std::cout << "[sync] === phase 1b: orphan cleanup ===\n";
    { OperationRecorder phase_rec("sync.phase.orphan_cleanup"); runOrphanCleanup(live); }

    // Phase 2: scraper matching — blocking so chapters don't race against it.
    std::cout << "[sync] === phase 2: scraper match ===\n";
    { OperationRecorder phase_rec("sync.phase.scraper_match"); if (scraper_) scraper_->runMatchSync(); }

    // Phase 2b: specials scan — opt-in per show, only for shows already matched.
    std::cout << "[sync] === phase 2b: specials scan ===\n";
    { OperationRecorder phase_rec("sync.phase.specials"); scanSpecialsForEligibleShows(); }

    // Phase 3: media probe (duration/resolution/languages) + subtitle sidecar scan.
    std::cout << "[sync] === phase 3: media probe ===\n";
    {
        OperationRecorder phase_rec("sync.phase.media_probe");
        for (const auto& src : sources_) {
            if (src->isSupported())
                syncMediaProbeFromFiles(src->sourceId());
        }
    }

    // Phase 4: smart playlist refresh — once per full cycle (not per-source,
    // unlike syncPlexLinks: a smart playlist isn't tied to any one source),
    // after every source's content is freshly ingested/matched so filters
    // see up-to-date data.
    std::cout << "[sync] === phase 4: smart playlist refresh ===\n";
    { OperationRecorder phase_rec("sync.phase.smart_playlists"); refreshSmartPlaylists(); }

    // Phase 5: chapter sync last because it's going to be the most time consuming.
    std::cout << "[sync] === phase 5: chapter sync ===\n";
    {
        OperationRecorder phase_rec("sync.phase.chapters");
        for (const auto& src : sources_) {
            if (src->isSupported())
                syncChaptersFromFiles(src->sourceId());
        }
    }

    // Printed last, not after phase 4 — chapter sync (phase 5) is the
    // longest-running phase by far (see this function's own comment above),
    // so reporting "done" before it ran told users sync had finished while
    // the slowest part was still grinding, and excluded its time from the
    // total.
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
    	
    	DLOG << "[sync-advanced] user discovery: " << source_id << " / " << users.size() << " users" << std::endl;

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
    syncMediaProbeFromFiles(source_id);
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

void SyncManager::applyWatchState(SQLite::Statement& s_get, SQLite::Statement& s_upsert_progress,
                                   SQLite::Statement& s_upsert_watched,
                                   const std::string& user_id, const std::string& item_type, const std::string& content_id,
                                   std::optional<bool> src_watched, std::optional<int64_t> src_view_count,
                                   std::optional<int64_t> src_position_ms, std::optional<int64_t> src_watched_at,
                                   int64_t duration_ms) {
    if (user_id.empty() || (!src_watched.has_value() && !src_position_ms.has_value())) return;

    s_get.reset();
    s_get.bind(1, user_id);
    s_get.bind(2, item_type);
    s_get.bind(3, content_id);
    const bool has_local = s_get.executeStep();
    const int64_t local_updated_at = has_local ? s_get.getColumn(0).getInt64() : 0;
    const int64_t local_count      = has_local ? s_get.getColumn(1).getInt64() : 0;

    const bool source_fresher = src_watched_at.has_value()
        ? (src_watched_at.value() > local_updated_at)
        : !has_local; // no timestamp from source: seed once, never overwrite
    if (!source_fresher) return;

    if (src_watched.value_or(false)) {
        // completed is the rewatch count itself (not a 0/1 flag) — take the
        // source's own count when it has one, but never regress below what's
        // already recorded locally.
        const int64_t incoming = std::max<int64_t>(src_view_count.value_or(1), 1);
        const int64_t merged   = std::max(local_count, incoming);
        s_upsert_watched.reset();
        s_upsert_watched.bind(1, user_id);
        s_upsert_watched.bind(2, item_type);
        s_upsert_watched.bind(3, content_id);
        s_upsert_watched.bind(4, duration_ms); // position clamped to full duration
        s_upsert_watched.bind(5, duration_ms);
        s_upsert_watched.bind(6, src_watched_at.value_or(static_cast<int64_t>(std::time(nullptr))));
        s_upsert_watched.bind(7, merged);
        s_upsert_watched.exec();
    } else if (src_position_ms.has_value()) {
        s_upsert_progress.reset();
        s_upsert_progress.bind(1, user_id);
        s_upsert_progress.bind(2, item_type);
        s_upsert_progress.bind(3, content_id);
        s_upsert_progress.bind(4, src_position_ms.value());
        s_upsert_progress.bind(5, duration_ms);
        s_upsert_progress.bind(6, src_watched_at.value_or(static_cast<int64_t>(std::time(nullptr))));
        s_upsert_progress.exec();
    }
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
	// Local-source native ids are raw filesystem paths (contain '/'), which
	// breaks httplib's path-param/regex-capture routes for anything built
	// from them. Plex/Jellyfin/Emby native ids (ratingKey/GUID) never have
	// this problem, so only local gets an opaque genID kairos_id here — the
	// real path still lives in source_mapping.external_id (and file_path)
	// for matching, so rescans of the same file keep resolving to the same
	// kairos_id via the tier-1/tier-2 lookups above.
	const bool is_local = src.sourceType() == "local";

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

    // primary_source + match_confirmed: which source currently owns each
    // show's metadata, and whether a human has confirmed a scraper match for
    // it, for the priority-gated merge in s_upsert_show below. Batch-loaded
    // up front like everything else here — no per-item reads during the
    // write phase.
    std::unordered_map<std::string, std::string> show_primary_source;
    std::unordered_set<std::string> show_match_confirmed;
    {
        SQLite::Statement q(sync_db_, "SELECT show_id, primary_source, match_confirmed FROM show");
        while (q.executeStep()) {
            show_primary_source[q.getColumn(0).getString()] = q.getColumn(1).getString();
            if (q.getColumn(2).getInt() != 0) show_match_confirmed.insert(q.getColumn(0).getString());
        }
    }
    // source_id -> sync_priority (lower wins); see priorityOf()/incomingWins() below.
    std::unordered_map<std::string, int> source_priority_by_id;
    {
        SQLite::Statement q(sync_db_, "SELECT source_id, sync_priority FROM media_source");
        while (q.executeStep())
            source_priority_by_id[q.getColumn(0).getString()] = q.getColumn(1).getInt();
    }
    // Unranked/unknown sources sort last (999999, same sentinel the old
    // added_at-only priority logic used) — an item with no configured
    // priority anywhere just keeps today's behavior of whichever source
    // touches it prevailing, so this is a no-op until a user actually ranks
    // their sources.
    auto priorityOf = [&](const std::string& sid) {
        auto it = source_priority_by_id.find(sid);
        return it != source_priority_by_id.end() ? it->second : 999999;
    };
    // True when this sync pass's own source should claim/overwrite an item
    // currently owned by current_owner — same-or-better priority always
    // wins (including a source re-affirming its own data every pass); a
    // strictly lower-priority source only backfills empty fields instead of
    // being locked out entirely (see s_upsert_show's per-field CASE).
    //
    // Once a human has confirmed a scraper match (match_confirmed — distinct
    // from `locked`, which only a manual field edit sets), a raw source can
    // no longer newly *claim* ownership away from whoever already holds it,
    // even by outranking them — it can still refresh fields it already
    // owns, and still backfill genuine gaps (that's the per-field CASE's
    // own, separate "current value is empty" branch, unaffected by this).
    // Without this, fixing the cross-ref-never-writes bug above would let a
    // higher-priority raw source silently overwrite a scraper's confirmed,
    // human-verified metadata the first time it happened to sync — the
    // exact "syncing overwrote a match the user made" outcome this guards.
    auto incomingWins = [&](const std::string& current_owner, bool match_confirmed) {
        if (match_confirmed && source_id != current_owner) return false;
        return priorityOf(source_id) <= priorityOf(current_owner);
    };

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

    // Note: duration validation, resolution, and embedded audio/subtitle
    // language probing all used to happen inline here (each its own ffprobe
    // spawn). They've moved to syncMediaProbeFromFiles — a dedicated
    // post-pass (like chapter probing already was) that runs one combined
    // ffprobe call per file needing any of the three, instead of up to
    // three separate spawns per file in this loop. The upsert below writes
    // whatever the source itself reported for duration_ms verbatim, and
    // leaves resolution_label/audio_languages/embedded_subtitle_languages
    // untouched (COALESCE'd against the existing DB value) for that pass to
    // fill in.

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
                kairos_id      = show_prefix + (is_local ? db::generateId() : ext_id);
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
    // Every field follows the same rule now (generalizing what used to be an
    // added_at-only special case, see s_upsert_movie's identical comment for
    // the full rationale): the incoming source overwrites unconditionally
    // when it's the same-or-higher priority than the item's current owner
    // (primary_source) — and claims ownership — otherwise it only backfills
    // fields the current owner left empty. incomingWins() above is computed
    // once per item in C++ (batch-loaded primary_source + sync_priority, no
    // per-row subqueries) and passed in as a single 0/1 bound once per field.
    SQLite::Statement s_upsert_show(sync_db_, R"(
        INSERT INTO show (show_id, title, content_rating, overview, studio, status,
                          genres, thumb, art, imdb_id, tvdb_id, tmdb_id,
                          originally_available_at, year, audience_rating,
                          labels, network, actors, countries, collections, folder_path,
                          added_at, added_at_source, primary_source, original_title)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(show_id) DO UPDATE SET
            title                   = CASE WHEN locked THEN title                   WHEN ? AND excluded.title<>''                   THEN excluded.title                   WHEN title=''                   THEN excluded.title                   ELSE title                   END,
            content_rating          = CASE WHEN locked THEN content_rating          WHEN ? AND excluded.content_rating<>''          THEN excluded.content_rating          WHEN content_rating=''          THEN excluded.content_rating          ELSE content_rating          END,
            overview                = CASE WHEN locked THEN overview                WHEN ? AND excluded.overview<>''                THEN excluded.overview                WHEN overview=''                THEN excluded.overview                ELSE overview                END,
            studio                  = CASE WHEN locked THEN studio                  WHEN ? AND excluded.studio<>''                  THEN excluded.studio                  WHEN studio=''                  THEN excluded.studio                  ELSE studio                  END,
            status                  = CASE WHEN locked THEN status                  WHEN ? AND excluded.status<>''                  THEN excluded.status                  WHEN status=''                  THEN excluded.status                  ELSE status                  END,
            genres                  = CASE WHEN locked THEN genres                  WHEN ? AND excluded.genres<>'' AND excluded.genres<>'[]'         THEN excluded.genres                  WHEN genres='' OR genres='[]'   THEN excluded.genres                  ELSE genres                  END,
            thumb                   = CASE WHEN locked THEN thumb                   WHEN ? AND excluded.thumb<>''                   THEN excluded.thumb                   WHEN thumb=''                   THEN excluded.thumb                   ELSE thumb                   END,
            art                     = CASE WHEN locked THEN art                     WHEN ? AND excluded.art<>''                     THEN excluded.art                     WHEN art=''                     THEN excluded.art                     ELSE art                     END,
            imdb_id                 = CASE WHEN locked THEN imdb_id                 WHEN ? AND excluded.imdb_id<>''                 THEN excluded.imdb_id                 WHEN imdb_id=''                 THEN excluded.imdb_id                 ELSE imdb_id                 END,
            tvdb_id                 = CASE WHEN locked THEN tvdb_id                 WHEN ? AND excluded.tvdb_id<>''                 THEN excluded.tvdb_id                 WHEN tvdb_id=''                 THEN excluded.tvdb_id                 ELSE tvdb_id                 END,
            tmdb_id                 = CASE WHEN locked THEN tmdb_id                 WHEN ? AND excluded.tmdb_id<>''                 THEN excluded.tmdb_id                 WHEN tmdb_id=''                 THEN excluded.tmdb_id                 ELSE tmdb_id                 END,
            originally_available_at = CASE WHEN locked THEN originally_available_at WHEN ? AND excluded.originally_available_at<>'' THEN excluded.originally_available_at WHEN originally_available_at='' THEN excluded.originally_available_at ELSE originally_available_at END,
            year                    = CASE WHEN locked THEN year                    WHEN ? AND excluded.year IS NOT NULL            THEN excluded.year                    WHEN year IS NULL               THEN excluded.year                    ELSE year                    END,
            audience_rating         = CASE WHEN locked THEN audience_rating         WHEN ? AND excluded.audience_rating IS NOT NULL THEN excluded.audience_rating         WHEN audience_rating IS NULL    THEN excluded.audience_rating         ELSE audience_rating         END,
            labels                  = CASE WHEN locked THEN labels                  WHEN ? AND excluded.labels<>'' AND excluded.labels<>'[]'         THEN excluded.labels                  WHEN labels='' OR labels='[]'   THEN excluded.labels                  ELSE labels                  END,
            network                 = CASE WHEN locked THEN network                 WHEN ? AND excluded.network<>''                 THEN excluded.network                 WHEN network=''                 THEN excluded.network                 ELSE network                 END,
            actors                  = CASE WHEN locked THEN actors                  WHEN ? AND excluded.actors<>'' AND excluded.actors<>'[]'         THEN excluded.actors                  WHEN actors='' OR actors='[]'   THEN excluded.actors                  ELSE actors                  END,
            countries               = CASE WHEN locked THEN countries               WHEN ? AND excluded.countries<>'' AND excluded.countries<>'[]'   THEN excluded.countries               WHEN countries='' OR countries='[]' THEN excluded.countries           ELSE countries               END,
            collections             = CASE WHEN locked THEN collections             WHEN ? AND excluded.collections<>'' AND excluded.collections<>'[]' THEN excluded.collections           WHEN collections='' OR collections='[]' THEN excluded.collections     ELSE collections             END,
            folder_path             = CASE WHEN locked THEN folder_path             WHEN ? AND excluded.folder_path<>''             THEN excluded.folder_path             WHEN folder_path=''             THEN excluded.folder_path             ELSE folder_path             END,
            added_at                = CASE WHEN locked THEN added_at                WHEN ? AND excluded.added_at IS NOT NULL        THEN excluded.added_at                WHEN added_at IS NULL           THEN excluded.added_at                ELSE added_at                END,
            added_at_source         = CASE WHEN locked THEN added_at_source         WHEN ? AND excluded.added_at IS NOT NULL        THEN excluded.added_at_source         WHEN added_at_source=''         THEN excluded.added_at_source         ELSE added_at_source         END,
            primary_source          = CASE WHEN locked THEN primary_source          WHEN ?                                                                              THEN excluded.primary_source                                                                                  ELSE primary_source          END,
            original_title          = CASE WHEN locked THEN original_title          WHEN ? AND excluded.original_title<>''          THEN excluded.original_title          WHEN original_title=''          THEN excluded.original_title          ELSE original_title          END
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
            folder_path             != excluded.folder_path             OR
            COALESCE(added_at,       -1) != COALESCE(excluded.added_at,       -1) OR
            added_at_source         != excluded.added_at_source         OR
            primary_source          != excluded.primary_source                     OR
            original_title          != excluded.original_title
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
                {
                    const auto owner_it = show_primary_source.find(show.show_id);
                    const std::string current_owner = owner_it != show_primary_source.end() ? owner_it->second : "";
                    const bool confirmed = show_match_confirmed.count(show.show_id) != 0;
                    const int wins = incomingWins(current_owner, confirmed) ? 1 : 0;

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
                    if (show.added_at.has_value()) s_upsert_show.bind(22, show.added_at.value());
                    else                            s_upsert_show.bind(22);
                    s_upsert_show.bind(23, show.added_at_source);
                    s_upsert_show.bind(24, source_id); // primary_source for a brand-new row
                    s_upsert_show.bind(25, show.original_title);
                    for (int p = 26; p <= 49; ++p) s_upsert_show.bind(p, wins);
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
            std::cout << "[sync-advanced]   wrote show metadata: "
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
        OperationRecorder::reportThreads(worker_count);
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(worker_count));
        for (int w = 0; w < worker_count; ++w) {
            workers.emplace_back([&]() {
                for (size_t i = next.fetch_add(1); i < shows.size(); i = next.fetch_add(1)) {
                    const auto t_fetch = std::chrono::steady_clock::now();
                    auto& eps = episodes_by_show[i] = src.fetchEpisodes(ext_show_ids[i]);
                    const long long fetch_ms = elapsedMs(t_fetch, std::chrono::steady_clock::now());

                    // duration_ms is written verbatim as the source reported it —
                    // validation, resolution, and embedded language probing all
                    // moved to syncMediaProbeFromFiles (see that function's
                    // comment for why this is now a dedicated post-pass instead
                    // of inline here).

                    std::lock_guard lock(s_log_mu);
                    std::cout << "[sync-advanced]     \"" << shows[i].title << "\": "
                              << eps.size() << " episode(s)" << std::endl;
                    DLOG << "[sync-advanced]       fetch=" << fetch_ms
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
        if (it != ep_ext_to_kairos.end()) return it->second;
		return ep_prefix + (is_local ? db::generateId() : ext);
	};
	auto ep_resolve_by_path = [&](const std::string& path) -> std::string {
        if (path.empty()) return "";
        std::string mapped = conf_.applyPathMap(path);
        auto it = ep_path_to_id.find(mapped);
        return it != ep_path_to_id.end() ? it->second : "";
    };

    // title/overview/thumb now follow the exact same confirmed-ownership
    // guard as s_upsert_show's own fields (see incomingWins()'s comment) —
    // previously these three were gated on `locked` alone, meaning ANY
    // source touching an episode on ANY sync pass could silently overwrite
    // its thumbnail/title/overview even when the parent show's match was
    // human-confirmed. Episodes have no match_confirmed of their own, so
    // `wins` here is computed from the PARENT SHOW's confirmed+ownership
    // state (see the per-show `wins` just above this episode loop) — a
    // wrong/lower-trust source (e.g. a Plex agent mismatching an obscure
    // episode) can no longer reclaim a thumb the confirmed source already
    // populated, the exact gap that let a bad poster silently resurface on
    // the next sync after being fixed.
    SQLite::Statement s_upsert_ep(sync_db_, R"(
        INSERT INTO episode (episode_id, show_id, season, episode, title,
                             file_path, duration_ms, overview, air_date,
                             thumb, absolute_index, resolution_label,
                             audio_languages, embedded_subtitle_languages)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(episode_id) DO UPDATE SET
            show_id        = excluded.show_id,
            season         = excluded.season,
            episode        = excluded.episode,
            title          = CASE WHEN locked THEN title    WHEN ? AND excluded.title<>''    THEN excluded.title    WHEN title=''    THEN excluded.title    ELSE title    END,
            file_path      = excluded.file_path,
            duration_ms    = excluded.duration_ms,
            overview       = CASE WHEN locked THEN overview WHEN ? AND excluded.overview<>'' THEN excluded.overview WHEN overview='' THEN excluded.overview ELSE overview END,
            air_date       = excluded.air_date,
            thumb          = CASE WHEN locked THEN thumb    WHEN ? AND excluded.thumb<>''    THEN excluded.thumb    WHEN thumb=''    THEN excluded.thumb    ELSE thumb    END,
            absolute_index = excluded.absolute_index,
            resolution_label            = COALESCE(NULLIF(excluded.resolution_label, ''), resolution_label),
            audio_languages             = COALESCE(NULLIF(excluded.audio_languages, '[]'), audio_languages),
            embedded_subtitle_languages = COALESCE(NULLIF(excluded.embedded_subtitle_languages, '[]'), embedded_subtitle_languages)
        WHERE (
            show_id        != excluded.show_id        OR
            season         != excluded.season         OR
            episode        != excluded.episode        OR
            file_path      != excluded.file_path      OR
            duration_ms    != excluded.duration_ms    OR
            air_date       != excluded.air_date       OR
            COALESCE(absolute_index, -1) != COALESCE(excluded.absolute_index, -1) OR
            (excluded.resolution_label != '' AND resolution_label != excluded.resolution_label) OR
            (excluded.audio_languages             != '[]' AND audio_languages             != excluded.audio_languages) OR
            (excluded.embedded_subtitle_languages  != '[]' AND embedded_subtitle_languages != excluded.embedded_subtitle_languages) OR
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
    SQLite::Statement s_watch_get(sync_db_, SyncManager::kWatchGetSql);
    SQLite::Statement s_watch_upsert_progress(sync_db_, SyncManager::kWatchUpsertProgressSql);
    SQLite::Statement s_watch_upsert_watched(sync_db_, SyncManager::kWatchUpsertWatchedSql);

    for (size_t i = 0; i < shows.size(); ++i) {
        auto& show     = shows[i];
        auto& episodes = episodes_by_show[i];
        const bool cross_show = cross_ref_shows[i];

        // Same incomingWins() computation s_upsert_show uses for this show,
        // reused for every one of its episodes below — see s_upsert_ep's own
        // comment for why episode title/overview/thumb need this too.
        const int ep_wins = [&] {
            const auto owner_it = show_primary_source.find(show.show_id);
            const std::string current_owner = owner_it != show_primary_source.end() ? owner_it->second : "";
            const bool confirmed = show_match_confirmed.count(show.show_id) != 0;
            return incomingWins(current_owner, confirmed) ? 1 : 0;
        }();

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
                    // Was ext_ep_id actually found in source_mapping? (Can't tell from
					// ep_kairos_id alone anymore — for a local source, a freshly-minted
					// id no longer has the deterministic ep_prefix+ext_ep_id shape that
					// used to double as a "not found" sentinel here.)
					if (!ep_ext_to_kairos.count(ext_ep_id))
					{
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
                        s_upsert_ep.bind(12, ep.resolution_label);
                        s_upsert_ep.bind(13, ep.audio_languages);
                        s_upsert_ep.bind(14, ep.embedded_subtitle_languages);
                        s_upsert_ep.bind(15, ep_wins);
                        s_upsert_ep.bind(16, ep_wins);
                        s_upsert_ep.bind(17, ep_wins);
                        s_upsert_ep.exec();
                    }
                    s_ep_mapping.reset();
                    s_ep_mapping.bind(1, ep.episode_id);
                    s_ep_mapping.bind(2, source_id);
                    s_ep_mapping.bind(3, library_id);
                    s_ep_mapping.bind(4, ext_ep_id);
                    s_ep_mapping.exec();

                    SyncManager::applyWatchState(s_watch_get, s_watch_upsert_progress, s_watch_upsert_watched,
                                     synced_user_id, "episode", ep.episode_id,
                                     ep.src_watched, ep.src_view_count, ep.src_position_ms, ep.src_watched_at, ep.duration_ms);
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
            std::cout << "[sync-advanced]   wrote series: \"" << show.title << "\" ("
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
    // See syncShows()'s identical comment on is_local/db::generateId().
    const bool is_local = src.sourceType() == "local";

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

    // Note: duration validation, resolution, and embedded audio/subtitle
    // language probing all moved to syncMediaProbeFromFiles — see
    // syncShows()'s identical comment for the full reasoning.

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

    // primary_source + match_confirmed + source priority — see syncShows()'s
    // identical setup for the full reasoning; incomingWins() drives
    // s_upsert_movie's priority-wins/lower-priority-backfills merge below.
    std::unordered_map<std::string, std::string> movie_primary_source;
    std::unordered_set<std::string> movie_match_confirmed;
    {
        SQLite::Statement q(sync_db_, "SELECT movie_id, primary_source, match_confirmed FROM movie");
        while (q.executeStep()) {
            movie_primary_source[q.getColumn(0).getString()] = q.getColumn(1).getString();
            if (q.getColumn(2).getInt() != 0) movie_match_confirmed.insert(q.getColumn(0).getString());
        }
    }
    std::unordered_map<std::string, int> source_priority_by_id;
    {
        SQLite::Statement q(sync_db_, "SELECT source_id, sync_priority FROM media_source");
        while (q.executeStep())
            source_priority_by_id[q.getColumn(0).getString()] = q.getColumn(1).getInt();
    }
    auto priorityOf = [&](const std::string& sid) {
        auto it = source_priority_by_id.find(sid);
        return it != source_priority_by_id.end() ? it->second : 999999;
    };
    auto incomingWins = [&](const std::string& current_owner, bool match_confirmed) {
        if (match_confirmed && source_id != current_owner) return false;
        return priorityOf(source_id) <= priorityOf(current_owner);
    };

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

    // duration_ms is written verbatim as the source reported it — validation,
    // resolution, and embedded language probing all moved to
    // syncMediaProbeFromFiles (see syncShows()'s identical comment).

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
                kairos_id = movie_prefix + (is_local ? db::generateId() : ext_id);
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

    // Every field follows the same priority-wins/lower-priority-backfills
    // rule now (generalizing what used to be an added_at-only special case,
    // and separately, writer/resolution_label's own non-priority-aware
    // "never let a blank clobber a set value" carve-out — both folded into
    // one consistent policy): the incoming source overwrites unconditionally
    // when it's the same-or-higher priority than the item's current owner
    // (primary_source) — and claims ownership — otherwise it only backfills
    // fields the current owner left empty. incomingWins() above is computed
    // once per item in C++ (batch-loaded primary_source + sync_priority, no
    // per-row subqueries) and passed in as a single 0/1 bound once per field.
    SQLite::Statement s_upsert_movie(sync_db_, R"(
        INSERT INTO movie (movie_id, title, content_rating, file_path, duration_ms, year,
                           overview, tagline, studio, director, writer, genres, thumb, art,
                           imdb_id, tmdb_id, audience_rating,
                           labels, actors, countries, collections,
                           added_at, added_at_source, resolution_label, primary_source, original_title,
                           audio_languages, embedded_subtitle_languages)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(movie_id) DO UPDATE SET
            title            = CASE WHEN locked THEN title            WHEN ? AND excluded.title<>''            THEN excluded.title            WHEN title=''                     THEN excluded.title            ELSE title            END,
            content_rating   = CASE WHEN locked THEN content_rating   WHEN ? AND excluded.content_rating<>''   THEN excluded.content_rating   WHEN content_rating=''            THEN excluded.content_rating   ELSE content_rating   END,
            file_path        = CASE WHEN locked THEN file_path        WHEN ? AND excluded.file_path<>''        THEN excluded.file_path        WHEN file_path=''                 THEN excluded.file_path        ELSE file_path        END,
            duration_ms      = CASE WHEN locked THEN duration_ms      WHEN ? AND excluded.duration_ms<>0       THEN excluded.duration_ms      WHEN duration_ms=0                THEN excluded.duration_ms      ELSE duration_ms      END,
            year             = CASE WHEN locked THEN year             WHEN ? AND excluded.year IS NOT NULL     THEN excluded.year             WHEN year IS NULL                 THEN excluded.year             ELSE year             END,
            overview         = CASE WHEN locked THEN overview         WHEN ? AND excluded.overview<>''         THEN excluded.overview         WHEN overview=''                  THEN excluded.overview         ELSE overview         END,
            tagline          = CASE WHEN locked THEN tagline          WHEN ? AND excluded.tagline<>''          THEN excluded.tagline          WHEN tagline=''                   THEN excluded.tagline          ELSE tagline          END,
            studio           = CASE WHEN locked THEN studio           WHEN ? AND excluded.studio<>''           THEN excluded.studio           WHEN studio=''                    THEN excluded.studio           ELSE studio           END,
            director         = CASE WHEN locked THEN director         WHEN ? AND excluded.director<>''         THEN excluded.director         WHEN director=''                  THEN excluded.director         ELSE director         END,
            writer           = CASE WHEN locked THEN writer           WHEN ? AND excluded.writer<>''           THEN excluded.writer           WHEN writer=''                    THEN excluded.writer           ELSE writer           END,
            genres           = CASE WHEN locked THEN genres           WHEN ? AND excluded.genres<>'' AND excluded.genres<>'[]'         THEN excluded.genres           WHEN genres='' OR genres='[]'     THEN excluded.genres           ELSE genres           END,
            thumb            = CASE WHEN locked THEN thumb            WHEN ? AND excluded.thumb<>''            THEN excluded.thumb            WHEN thumb=''                     THEN excluded.thumb            ELSE thumb            END,
            art              = CASE WHEN locked THEN art              WHEN ? AND excluded.art<>''              THEN excluded.art              WHEN art=''                       THEN excluded.art              ELSE art              END,
            imdb_id          = CASE WHEN locked THEN imdb_id          WHEN ? AND excluded.imdb_id<>''          THEN excluded.imdb_id          WHEN imdb_id=''                   THEN excluded.imdb_id          ELSE imdb_id          END,
            tmdb_id          = CASE WHEN locked THEN tmdb_id          WHEN ? AND excluded.tmdb_id<>''          THEN excluded.tmdb_id          WHEN tmdb_id=''                   THEN excluded.tmdb_id          ELSE tmdb_id          END,
            audience_rating  = CASE WHEN locked THEN audience_rating  WHEN ? AND excluded.audience_rating IS NOT NULL THEN excluded.audience_rating WHEN audience_rating IS NULL THEN excluded.audience_rating  ELSE audience_rating  END,
            labels           = CASE WHEN locked THEN labels           WHEN ? AND excluded.labels<>'' AND excluded.labels<>'[]'         THEN excluded.labels           WHEN labels='' OR labels='[]'     THEN excluded.labels           ELSE labels           END,
            actors           = CASE WHEN locked THEN actors           WHEN ? AND excluded.actors<>'' AND excluded.actors<>'[]'         THEN excluded.actors           WHEN actors='' OR actors='[]'     THEN excluded.actors           ELSE actors           END,
            countries        = CASE WHEN locked THEN countries        WHEN ? AND excluded.countries<>'' AND excluded.countries<>'[]'   THEN excluded.countries        WHEN countries='' OR countries='[]' THEN excluded.countries       ELSE countries        END,
            collections      = CASE WHEN locked THEN collections      WHEN ? AND excluded.collections<>'' AND excluded.collections<>'[]' THEN excluded.collections    WHEN collections='' OR collections='[]' THEN excluded.collections ELSE collections      END,
            added_at         = CASE WHEN locked THEN added_at         WHEN ? AND excluded.added_at IS NOT NULL THEN excluded.added_at         WHEN added_at IS NULL             THEN excluded.added_at         ELSE added_at         END,
            added_at_source  = CASE WHEN locked THEN added_at_source  WHEN ? AND excluded.added_at IS NOT NULL THEN excluded.added_at_source  WHEN added_at_source=''           THEN excluded.added_at_source  ELSE added_at_source  END,
            resolution_label = CASE WHEN locked THEN resolution_label WHEN ? AND excluded.resolution_label<>'' THEN excluded.resolution_label WHEN resolution_label=''          THEN excluded.resolution_label ELSE resolution_label END,
            primary_source   = CASE WHEN locked THEN primary_source   WHEN ?                                   THEN excluded.primary_source                                                                     ELSE primary_source   END,
            original_title   = CASE WHEN locked THEN original_title   WHEN ? AND excluded.original_title<>''   THEN excluded.original_title   WHEN original_title=''            THEN excluded.original_title   ELSE original_title   END,
            audio_languages             = CASE WHEN locked THEN audio_languages             WHEN ? AND excluded.audio_languages<>'' AND excluded.audio_languages<>'[]'                         THEN excluded.audio_languages             WHEN audio_languages=''             OR audio_languages='[]'             THEN excluded.audio_languages             ELSE audio_languages             END,
            embedded_subtitle_languages = CASE WHEN locked THEN embedded_subtitle_languages WHEN ? AND excluded.embedded_subtitle_languages<>'' AND excluded.embedded_subtitle_languages<>'[]' THEN excluded.embedded_subtitle_languages WHEN embedded_subtitle_languages='' OR embedded_subtitle_languages='[]' THEN excluded.embedded_subtitle_languages ELSE embedded_subtitle_languages END
        WHERE NOT locked AND (
            title           != excluded.title           OR
            content_rating  != excluded.content_rating  OR
            file_path       != excluded.file_path       OR
            duration_ms     != excluded.duration_ms     OR
            overview        != excluded.overview        OR
            tagline         != excluded.tagline         OR
            studio          != excluded.studio          OR
            director        != excluded.director        OR
            writer          != excluded.writer          OR
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
            collections     != excluded.collections     OR
            COALESCE(added_at,       -1) != COALESCE(excluded.added_at,       -1) OR
            added_at_source != excluded.added_at_source OR
            resolution_label != excluded.resolution_label OR
            primary_source  != excluded.primary_source  OR
            original_title  != excluded.original_title  OR
            audio_languages             != excluded.audio_languages             OR
            embedded_subtitle_languages != excluded.embedded_subtitle_languages
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
    SQLite::Statement s_watch_get(sync_db_, SyncManager::kWatchGetSql);
    SQLite::Statement s_watch_upsert_progress(sync_db_, SyncManager::kWatchUpsertProgressSql);
    SQLite::Statement s_watch_upsert_watched(sync_db_, SyncManager::kWatchUpsertWatchedSql);

    for (size_t batch_start = 0; batch_start < movies.size(); batch_start += kMovieBatchSize) {
        yieldIfRequested();
        const size_t batch_end = std::min(batch_start + kMovieBatchSize, movies.size());
        try {
            SQLite::Transaction txn(sync_db_, SQLite::TransactionBehavior::IMMEDIATE);
            for (size_t i = batch_start; i < batch_end; ++i) {
                const auto& movie = movies[i];
                const auto& res   = resolved[i];

                {
                    const auto owner_it = movie_primary_source.find(movie.movie_id);
                    const std::string current_owner = owner_it != movie_primary_source.end() ? owner_it->second : "";
                    const bool confirmed = movie_match_confirmed.count(movie.movie_id) != 0;
                    const int wins = incomingWins(current_owner, confirmed) ? 1 : 0;

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
                    s_upsert_movie.bind(11, movie.writer);
                    s_upsert_movie.bind(12, movie.genres);
                    s_upsert_movie.bind(13, movie.thumb);
                    s_upsert_movie.bind(14, movie.art);
                    s_upsert_movie.bind(15, movie.imdb_id);
                    s_upsert_movie.bind(16, movie.tmdb_id);
                    if (movie.audience_rating.has_value()) s_upsert_movie.bind(17, movie.audience_rating.value());
                    else                                   s_upsert_movie.bind(17);
                    s_upsert_movie.bind(18, movie.labels);
                    s_upsert_movie.bind(19, movie.actors);
                    s_upsert_movie.bind(20, movie.countries);
                    s_upsert_movie.bind(21, movie.collections);
                    if (movie.added_at.has_value()) s_upsert_movie.bind(22, movie.added_at.value());
                    else                             s_upsert_movie.bind(22);
                    s_upsert_movie.bind(23, movie.added_at_source);
                    s_upsert_movie.bind(24, movie.resolution_label);
                    s_upsert_movie.bind(25, source_id); // primary_source for a brand-new row
                    s_upsert_movie.bind(26, movie.original_title);
                    s_upsert_movie.bind(27, movie.audio_languages);
                    s_upsert_movie.bind(28, movie.embedded_subtitle_languages);
                    // 27 wins-flag placeholders (one per SET column above) at
                    // positions 29..55. Was previously 27..50 (24 of the then
                    // 25 needed binds) — off by one, leaving the last column's
                    // (original_title's) wins-flag permanently unbound/NULL,
                    // so original_title could never be won over by a higher-
                    // priority source, only backfilled when blank. Fixed here
                    // while extending this statement for the 2 new columns.
                    for (int p = 29; p <= 55; ++p) s_upsert_movie.bind(p, wins);
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

                SyncManager::applyWatchState(s_watch_get, s_watch_upsert_progress, s_watch_upsert_watched,
                                 synced_user_id, "movie", movie.movie_id,
                                 movie.src_watched, movie.src_view_count, movie.src_position_ms, movie.src_watched_at, movie.duration_ms);
            }
            txn.commit();
            std::cout << "[sync-advanced]   wrote movies: "
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
    DLOG << "[sync-advanced] yielding — coordinator requested DB write window\n";
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
    TaskRegistry::global().spawn([this]() {
        try {
            for (const auto& src : sources_)
                syncPlexLinks(src->sourceId());
        } catch (const std::exception& e) {
            std::cerr << "[sync] plex-link sync error: " << e.what() << std::endl;
        }
        plex_sync_running_.store(false);
    });
}

void SyncManager::triggerSmartPlaylistRefresh() {
    bool expected = false;
    if (!smart_playlist_refresh_running_.compare_exchange_strong(expected, true)) {
        std::cout << "[sync] smart playlist refresh already running — ignoring trigger" << std::endl;
        return;
    }
    TaskRegistry::global().spawn([this]() {
        try {
            refreshSmartPlaylists();
        } catch (const std::exception& e) {
            std::cerr << "[sync] smart playlist refresh error: " << e.what() << std::endl;
        }
        smart_playlist_refresh_running_.store(false);
    });
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
    SQLite::Statement s_watch_get(sync_db_, SyncManager::kWatchGetSql);
    SQLite::Statement s_watch_upsert_progress(sync_db_, SyncManager::kWatchUpsertProgressSql);
    SQLite::Statement s_watch_upsert_watched(sync_db_, SyncManager::kWatchUpsertWatchedSql);

    for (const auto& lu : linked) {
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
                	DLOG << "[sync-advanced] linked user " << lu.external_user_id << " has no mapping for " << e.item_type << " " << e.external_id << '\n';
	                continue; // not in an enabled library
                }
                const std::string kairos_id = s_resolve.getColumn(0).getString();

                int64_t duration_ms = 0;
                SQLite::Statement& s_duration = (e.item_type == "movie") ? s_duration_mv : s_duration_ep;
                s_duration.reset();
                s_duration.bind(1, kairos_id);
                if (s_duration.executeStep()) duration_ms = s_duration.getColumn(0).getInt64();

            	DLOG << "[sync-advanced] syncing linked user watch state for " << e.title << " " << kairos_id << '\n';
                SyncManager::applyWatchState(s_watch_get, s_watch_upsert_progress, s_watch_upsert_watched,
                                 lu.local_user_id, e.item_type, kairos_id,
                                 e.watched, e.view_count, e.position_ms, e.watched_at, duration_ms);
            }
            txn.commit();
        } catch (const std::exception& ex) {
            std::cerr << "[sync] linked-user watch state error (source=" << source_id
                      << " user=" << lu.external_user_id << "): " << ex.what() << '\n';
        }
    }
}

void SyncManager::syncPlexLinks(const std::string& source_id) {
    // Dispatched through IMediaSource (browsePlaylistItems/browseCollectionItems)
    // rather than a hand-rolled Plex-only httplib::Client — works for Plex,
    // Jellyfin, and Emby alike (all three implement the same browse methods),
    // instead of silently no-op'ing for every non-Plex source (the old
    // `if (source_type != "plex") return;` guard) and re-duplicating Plex's
    // own pagination fix a third time in this one file. sources_/findSource
    // only ever holds already-loaded, enabled sources (see loadSources()), so
    // no separate enabled/base_url check is needed here.
    IMediaSource* src = findSource(source_id);
    if (!src) return;

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

    std::cout << "[sync] re-syncing " << links.size() << " linked list(s) (source=" << source_id << ")" << std::endl;

    for (const auto& link : links) {
		// Self-heal stale links: plex_list_link has no FK back to playlist/
		// filler_list (see PlaylistRepository::remove()'s own comment on this
		// exact hazard), so a link whose target was deleted through a path
		// that doesn't clean it up (e.g. POST /api/config/library/reset, which
		// wipes `playlist` via raw SQL instead of going through
		// PlaylistRepository::remove()) survives and would otherwise fail the
		// real FK constraint on playlist_item/filler_list_item on every
		// single sync, forever, with no way to recover short of a manual DB
		// fix. Detect and remove the orphan here instead of retrying
		// indefinitely.
		const std::string parent_tbl = (link.list_type == "playlist") ? "playlist" : "filler_list";
		const std::string parent_col = (link.list_type == "playlist") ? "playlist_id" : "filler_list_id";
		{
			SQLite::Statement chk(sync_db_, "SELECT 1 FROM " + parent_tbl + " WHERE " + parent_col + " = ?");
			chk.bind(1, link.list_id);
			if (!chk.executeStep())
			{
				std::cerr << "[sync] linked " << link.list_type << " \"" << link.list_id
					<< "\" no longer exists — removing its stale Plex link" << std::endl;
				SQLite::Statement del_link(sync_db_,
										   "DELETE FROM plex_list_link WHERE list_type = ? AND list_id = ?");
				del_link.bind(1, link.list_type);
				del_link.bind(2, link.list_id);
				del_link.exec();
				continue;
			}
		}
		try {
            auto raw = (link.plex_type == "collection")
                ? src->browseCollectionItems(link.external_id)
                : src->browsePlaylistItems(link.external_id);

            struct Item { std::string item_type; std::string kairos_id; };
            std::vector<Item> items;
            for (const auto& ri : raw) {
                SQLite::Statement lk(sync_db_,
                    "SELECT kairos_id FROM source_mapping "
                    "WHERE source_id=? AND external_id=? AND item_type=?");
                lk.bind(1, source_id); lk.bind(2, ri.external_id); lk.bind(3, ri.item_type);
                if (!lk.executeStep()) continue;
                const std::string kairos_id = lk.getColumn(0).getString();
                if (ri.item_type != "show") { items.push_back({ri.item_type, kairos_id}); continue; }
                // A collection can contain whole shows (a common Plex/
                // Jellyfin use case), but playlist_item only supports
                // 'episode'/'movie' — expand to every episode instead of
                // adding the show directly, same as PlexSyncHelper.cpp's
                // resolveAndExpand (duplicated here for the same main-
                // thread-vs-background-thread reason as syncPlexLinks itself).
                SQLite::Statement eq(sync_db_,
                    "SELECT episode_id FROM episode WHERE show_id = ? AND season > 0 ORDER BY season, episode");
                eq.bind(1, kairos_id);
                while (eq.executeStep()) items.push_back({"episode", eq.getColumn(0).getString()});
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
// Smart playlist refresh
// ---------------------------------------------------------------------------

namespace {
// Mirrors PlaylistRepository.cpp's identical (private) helpers — duplicated
// rather than shared because this runs against sync_db_ (a raw background-
// thread connection) while PlaylistRepository is bound to the main-thread
// Database&; same reasoning as syncPlexLinks/syncSourceListItems's existing
// main-thread-vs-background-thread duplication.
struct SmartSortFieldSql
{
	std::string expr;
	bool desc;
};

SmartSortFieldSql smartSortFieldSql(const std::string& sort, const std::string& alias, bool isShow)
{
	if (sort == "recently_added") return {alias + ".added_at", true};
	if (sort == "year") return {"COALESCE(" + alias + ".year, 0)", true};
	if (sort == "audience_rating") return {"COALESCE(" + alias + ".audience_rating, 0)", true};
	if (sort == "duration")
		return isShow
				   ? SmartSortFieldSql{"COALESCE((SELECT AVG(e_d.duration_ms) FROM episode e_d WHERE e_d.show_id = " + alias + ".show_id), 0)", true}
				   : SmartSortFieldSql{alias + ".duration_ms", true};
	if (sort == "recently_released_or_aired")
		return isShow
				   ? SmartSortFieldSql{
					   "COALESCE((SELECT MAX(e2.air_date) FROM episode e2 WHERE e2.show_id = " + alias + ".show_id "
					   "AND e2.air_date != '' AND e2.air_date <= date('now')), '0000-00-00')",
					   true
				   }
				   : SmartSortFieldSql{"COALESCE(NULLIF(" + alias + ".release_date, ''), '0000-00-00')", true};
	if (sort == "air_date")
		return isShow
				   ? SmartSortFieldSql{"COALESCE(NULLIF(" + alias + ".originally_available_at, ''), '9999-99-99')", false}
				   : SmartSortFieldSql{"COALESCE(NULLIF(" + alias + ".release_date, ''), '9999-99-99')", false};
	if (sort == "random") return {"RANDOM()", false};
	return {alias + ".title", false}; // default: title ASC
}

std::string smartOrderBySql(const std::string& sort, const std::string& alias, bool isShow, const std::string& sort_dir = "")
{
	auto f    = smartSortFieldSql(sort, alias, isShow);
	bool desc = sort_dir.empty() ? f.desc : (sort_dir == "desc");
	return " ORDER BY " + f.expr + (desc ? " DESC" : " ASC");
}
}

void SyncManager::refreshSmartPlaylists() {
    const auto t_start = std::chrono::steady_clock::now();
    struct SmartDef {
        std::string playlist_id, filter_expr, smart_type, smart_sort, smart_sort_dir;
		int smart_limit;
		bool smart_expand_episodes;
	};
	std::vector<SmartDef> defs;
	{
		SQLite::Statement q(sync_db_,
							"SELECT playlist_id, filter_expr, smart_type, smart_sort, smart_limit, smart_sort_dir, smart_expand_episodes "
							"FROM playlist WHERE membership = 'smart'");
		while (q.executeStep())
		{
			defs.push_back({
				q.getColumn(0).getString(), q.getColumn(1).getString(),
				q.getColumn(2).getString(), q.getColumn(3).getString(),
                q.getColumn(5).getString(),
                q.getColumn(4).getInt(), q.getColumn(6).getInt() != 0
			});
		}
	}
	if (defs.empty())
	{
		std::cout << "[sync] smart playlist refresh: nothing to refresh (no smart playlists defined)" << std::endl;
		return;
	}

	std::cout << "[sync] refreshing " << defs.size() << " smart playlist(s)" << std::endl;

    for (const auto& def : defs) {
        try {
            const std::string limit_clause = def.smart_limit > 0 ? " LIMIT " + std::to_string(def.smart_limit) : "";
            struct Item { std::string item_type, item_id; };
            std::vector<Item> items;

            if (def.smart_type == "mixed") {
                // mixedEntityOrder already returns movie/episode pairs (not
                // shows) — see its header comment — so no per-show expansion
				// loop is needed. No specific viewer on this background
				// pass, so watch_state stays unresolved (empty user_id),
				// same as the branches below.
				for (const auto& [entity_type, id] : mixedEntityOrder(sync_db_, def.filter_expr, def.smart_sort, def.smart_limit, def.smart_sort_dir, ""))
				{
					items.push_back({entity_type, id});
				}
			}
			else if (def.smart_type == "movie")
			{
				auto compiled = compileFilterExpr(def.filter_expr, FilterEntity::Movie, "m");
				SQLite::Statement q(sync_db_,
									"SELECT m.movie_id FROM movie m WHERE (" + compiled.sql + ")" +
									smartOrderBySql(def.smart_sort, "m", false, def.smart_sort_dir) + limit_clause);
				for (size_t i = 0; i < compiled.binds.size(); ++i) q.bind(static_cast<int>(i) + 1, compiled.binds[i]);
				while (q.executeStep()) items.push_back({"movie", q.getColumn(0).getString()});
			}
			else if (def.smart_expand_episodes)
			{
				// See PlaylistRepository::refreshSmart's identical branch —
				// flattens to a per-episode list under `smart_sort` (any
				// mode) instead of shows-then-dump-each-show's-entire-run.
				// smart_limit is an episode cap here. Unlike refreshSmart,
				// this background pass has no specific viewer, so a
				// `watch_state:X` clause in the filter stays a no-op here
				// (fetchMixedEpisodeRows's user_id is empty) — only the
				// interactive "Save & Refresh" button resolves it.
				auto episodes = fetchMixedEpisodeRows(sync_db_, def.filter_expr, "");
				bool desc     = def.smart_sort_dir.empty() ? mixedRowNaturalDesc(def.smart_sort) : (def.smart_sort_dir == "desc");
				sortMixedRows(episodes, def.smart_sort, desc);
				if (def.smart_limit > 0 && static_cast<int>(episodes.size()) > def.smart_limit) episodes.resize(def.smart_limit);
				for (auto& ep : episodes) items.push_back({"episode", ep.id});
			}
			else
			{
				// Same no-viewer caveat as the flattened branch above —
				// watch_state stays unresolved (no-op) for this background
				// pass regardless of sort mode.
				auto compiled = compileFilterExpr(def.filter_expr, FilterEntity::Show, "s");
				SQLite::Statement q(sync_db_,
									"SELECT s.show_id FROM show s WHERE (" + compiled.sql + ")" +
									smartOrderBySql(def.smart_sort, "s", true, def.smart_sort_dir) + limit_clause);
				for (size_t i = 0; i < compiled.binds.size(); ++i) q.bind(static_cast<int>(i) + 1, compiled.binds[i]);
				std::vector<std::string> show_ids;
				while (q.executeStep()) show_ids.push_back(q.getColumn(0).getString());

				for (const auto& show_id : show_ids)
				{
					SQLite::Statement eq(sync_db_,
										 "SELECT episode_id FROM episode WHERE show_id = ? AND season > 0 ORDER BY season, episode");
					eq.bind(1, show_id);
					while (eq.executeStep()) items.push_back({"episode", eq.getColumn(0).getString()});
				}
			}

			SQLite::Transaction txn(sync_db_, SQLite::TransactionBehavior::IMMEDIATE);
            SQLite::Statement del(sync_db_, "DELETE FROM playlist_item WHERE playlist_id = ?");
            del.bind(1, def.playlist_id); del.exec();

            int pos = 0;
            for (const auto& item : items) {
                SQLite::Statement ins(sync_db_,
                    "INSERT OR IGNORE INTO playlist_item (playlist_id, position, item_type, item_id) VALUES (?,?,?,?)");
                ins.bind(1, def.playlist_id); ins.bind(2, pos++);
                ins.bind(3, item.item_type); ins.bind(4, item.item_id);
                ins.exec();
            }

            SQLite::Statement ts(sync_db_, "UPDATE playlist SET last_smart_refresh_at = ? WHERE playlist_id = ?");
            ts.bind(1, static_cast<int64_t>(std::time(nullptr))); ts.bind(2, def.playlist_id);
            ts.exec();

            txn.commit();
            std::cout << "[sync]   \"" << def.playlist_id << "\": " << items.size() << " item(s)" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[sync] error refreshing smart playlist " << def.playlist_id
                      << ": " << e.what() << std::endl;
        }
    }
    std::cout << "[sync] smart playlist refresh done: " << defs.size() << " playlist(s) ("
              << elapsedMs(t_start, std::chrono::steady_clock::now()) << "ms)" << std::endl;
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

// Probes only items with no existing 'file'-sourced chapter row — i.e. items
// this pass hasn't chaptered before, not a full re-probe of the source every
// sync. Revisit this gating once our own chapter-detection algorithm (see
// ChapterDetector.h) replaces raw ffprobe markers as the primary source.
void SyncManager::syncChaptersFromFiles(const std::string& source_id) {
    {
        SQLite::Statement q(sync_db_,
            "SELECT value FROM app_config WHERE key='chapter_sync_enabled'");
        if (q.executeStep() && q.getColumn(0).getString() == "false") return;
    }

    OperationRecorder op_rec("chapters.sync_from_files");
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
              AND NOT EXISTS (
                  SELECT 1 FROM chapter c
                  WHERE c.media_type='episode' AND c.media_id=sm.kairos_id AND c.source='file'
              )
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
              AND NOT EXISTS (
                  SELECT 1 FROM chapter c
                  WHERE c.media_type='movie' AND c.media_id=sm.kairos_id AND c.source='file'
              )
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
        OperationRecorder::reportThreads(worker_count);
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

namespace
{
	// Size + mtime at keyframe-probe time — stored alongside keyframes_ms so
	// Hephaestus can tell a cached probe is still trustworthy without assuming a
	// stable file_path means a stable file: Sonarr/Radarr-style upgrades replace
	// a library file in place (same path, new content), which this catches but
	// the path alone never would. Same last_write_time-to-epoch conversion as
	// LocalSource.cpp's fsAddedAtEpoch, kept as a separate copy since that one's
	// file-local. Both fields are 0 on any stat error, which a real file's stat
	// will practically never match, so a stat failure here just always forces
	// Hephaestus to re-probe rather than risking a false "unchanged."
	struct FileFingerprint
	{
		int64_t size = 0, mtime = 0;
	};

	FileFingerprint statFingerprint(const std::filesystem::path& p)
	{
		std::error_code ec;
		FileFingerprint fp;
		auto sz = std::filesystem::file_size(p, ec);
		if (!ec) fp.size = static_cast<int64_t>(sz);
		auto ftime = std::filesystem::last_write_time(p, ec);
		if (!ec)
		{
			auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
			fp.mtime  = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count());
		}
		return fp;
	}
} // namespace

// See SyncManager.h's declaration for the full rationale (combined ffprobe +
// subtitle sidecar cataloging in one pass over the same item set).
void SyncManager::syncMediaProbeFromFiles(const std::string& source_id) {
    const auto t_start = std::chrono::steady_clock::now();
    std::cout << "[sync] media probe + subtitle sidecar scan: " << source_id << std::endl;

    struct ScanItem {
        std::string kairos_id;
        std::string media_type;      // "episode" | "movie"
        std::string unmapped_path;
        std::string mapped_path;
        std::string mapped_dir;
        std::string video_stem;
        std::string resolution_label; // current DB value — empty means never probed
        int64_t     duration_ms = 0;  // current DB value
        std::string audio_languages;  // current DB value — '[]'/empty means never (language-)probed
		std::string keyframes_ms;     // current DB value — empty means never keyframe-probed
	};
	std::vector<ScanItem> items;

    {
        SQLite::Statement q(sync_db_, R"(
            SELECT sm.kairos_id, e.file_path, e.resolution_label, e.duration_ms, e.audio_languages, e.keyframes_ms
            FROM source_mapping sm
            JOIN episode e ON e.episode_id = sm.kairos_id
            WHERE sm.item_type='episode' AND sm.source_id=?
        )");
		q.bind(1, source_id);
        while (q.executeStep()) {
            const std::string file_path = q.getColumn(1).getString();
            if (file_path.empty()) continue;
            const std::string mapped = conf_.applyPathMap(file_path);
            if (!std::filesystem::exists(mapped)) continue;
            std::filesystem::path mp(mapped);
            items.push_back({
                q.getColumn(0).getString(), "episode", file_path, mapped,
                mp.parent_path().string(), mp.filename().stem().string(),
                q.getColumn(2).getString(), q.getColumn(3).getInt64(), q.getColumn(4).getString(),
				q.getColumn(5).getString()
			});
		}
	}
	{
		SQLite::Statement q(sync_db_, R"(
            SELECT sm.kairos_id, m.file_path, m.resolution_label, m.duration_ms, m.audio_languages, m.keyframes_ms
            FROM source_mapping sm
            JOIN movie m ON m.movie_id = sm.kairos_id
            WHERE sm.item_type='movie' AND sm.source_id=?
        )");
		q.bind(1, source_id);
		while (q.executeStep())
		{
			const std::string file_path = q.getColumn(1).getString();
            if (file_path.empty()) continue;
            const std::string mapped = conf_.applyPathMap(file_path);
            if (!std::filesystem::exists(mapped)) continue;
            std::filesystem::path mp(mapped);
            items.push_back({
                q.getColumn(0).getString(), "movie", file_path, mapped,
                mp.parent_path().string(), mp.filename().stem().string(),
                q.getColumn(2).getString(), q.getColumn(3).getInt64(), q.getColumn(4).getString(),
				q.getColumn(5).getString()
			});
		}
	}

	if (items.empty())
	{
		std::cout << "[sync] media probe done: " << source_id << " (nothing to scan)" << std::endl;
        return;
    }

    // ── Subtitle sidecar scan — group by directory, list each exactly once ──
    std::unordered_map<std::string, std::vector<std::string>> dir_to_stems;
    for (const auto& it : items) dir_to_stems[it.mapped_dir].push_back(it.video_stem);

    std::vector<std::string> dirs;
    dirs.reserve(dir_to_stems.size());
    for (auto& [dir, stems] : dir_to_stems) dirs.push_back(dir);

    std::vector<std::unordered_map<std::string, std::vector<ExternalSubtitle>>> dir_results(dirs.size());
    {
        std::atomic<size_t> next{0};
        const int worker_count = std::min<int>(getThreadCount(), static_cast<int>(dirs.size()));
        OperationRecorder::reportThreads(worker_count);
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(worker_count));
        for (int w = 0; w < worker_count; ++w) {
            workers.emplace_back([&]() {
                for (size_t d = next.fetch_add(1); d < dirs.size(); d = next.fetch_add(1))
                    dir_results[d] = scanDirectoryForSubtitles(dirs[d], dir_to_stems.at(dirs[d]));
            });
        }
        for (auto& t : workers) t.join();
    }
    std::unordered_map<std::string, size_t> dir_index;
    for (size_t d = 0; d < dirs.size(); ++d) dir_index[dirs[d]] = d;

    // ── Media-info probe — one combined ffprobe call per file that needs it ──
    // (resolution never probed, the current duration looks implausible,
    // audio_languages is still at its untouched '[]'/empty default, or
	// keyframes_ms is still empty). The language check matters on its own: a
	// file can arrive with a valid resolution_label/duration_ms from the
	// source's own metadata (e.g. Plex-reported values on import) well
	// before Kairos ever ffprobes it itself, which used to leave
	// audio_languages/embedded_subtitle_languages permanently unpopulated
	// for such files — probeFileInfo (the only thing that fills them in)
	// would never run because the other two fields already looked "already
	// probed." Surfaced as audio/subtitle language pills and the
	// pre-playback language picker silently having nothing to show for an
	// otherwise perfectly normal file. The keyframes_ms check is the same
	// idea, and also what backfills it for every file synced before that
	// column existed (see Database.cpp's v98 migration comment).
	std::vector<std::optional<FileProbeInfo>> probe_results(items.size());
	{
		std::atomic<size_t> next{0};
		const int worker_count = std::min<int>(getThreadCount(), static_cast<int>(items.size()));
        OperationRecorder::reportThreads(worker_count);
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(worker_count));
        for (int w = 0; w < worker_count; ++w) {
            workers.emplace_back([&]() {
                for (size_t i = next.fetch_add(1); i < items.size(); i = next.fetch_add(1)) {
                    const auto& it = items[i];
                    const bool langs_never_probed = it.audio_languages.empty() || it.audio_languages == "[]";
                    const bool needs_probe = it.resolution_label.empty() || !durationLooksValid(it.duration_ms)
                        || langs_never_probed || it.keyframes_ms.empty();
					if (needs_probe) probe_results[i] = probeFileInfo(it.mapped_path);
				}
			});
		}
		for (auto& t : workers) t.join();
	}

	// ── Compute (in memory, no DB I/O) ─────────────────────────────────────────
    struct ProbeUpdate {
        std::string kairos_id;
        std::string resolution_label;
        std::string audio_languages;
        std::string embedded_subs;
        int64_t     duration_ms;
        std::string keyframes_ms;
        int64_t     keyframes_size;
        int64_t     keyframes_mtime;
    };
    std::vector<std::string> episode_ids, movie_ids;
    std::vector<SubtitleTrack> episode_tracks, movie_tracks;
    std::vector<ProbeUpdate> episode_updates, movie_updates;
	int with_ext_subs = 0, media_probed = 0;
	// How many items will actually get an ffprobe this pass — for the "N/M"
    // progress prefix below, so a user watching the Activity log can tell how
    // much of this phase is left, not just that it's running.
    const size_t to_probe = static_cast<size_t>(std::count_if(
        probe_results.begin(), probe_results.end(), [](const auto& o) { return o.has_value(); }));

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        auto& id_list = it.media_type == "movie" ? movie_ids : episode_ids;
        id_list.push_back(it.kairos_id);

        const auto& dir_result = dir_results[dir_index.at(it.mapped_dir)];
        bool has_ext_subs_this_item = false;
        if (auto match = dir_result.find(it.video_stem); match != dir_result.end()) {
            has_ext_subs_this_item = true;
            auto& track_list = it.media_type == "movie" ? movie_tracks : episode_tracks;
            const std::string unmapped_dir = pathutil::parentDir(it.unmapped_path);
            for (auto& ext : match->second) {
                SubtitleTrack t;
                t.media_type = it.media_type;
                t.media_id   = it.kairos_id;
                t.file_path  = unmapped_dir + "/" + std::filesystem::path(ext.file_path).filename().string();
                t.language   = ext.language;
                t.forced     = ext.forced;
                t.sdh        = ext.sdh;
                t.title      = ext.title;
                // ext.file_path is the mapped path (see SubtitleSidecar.h),
                // actually readable from this process — matching the
                // filename convention doesn't mean the file is a real,
                // non-empty/non-corrupt subtitle track (see
                // SubtitleValidation.h's own comment for a real example).
                // Still recorded either way (with valid=0), not silently
                // dropped, so the admin's Review > Subtitles tab has
                // something to show and fix on disk.
                auto validation = validateSubtitleFile(ext.file_path);
                t.valid          = validation.valid;
                t.invalid_reason = validation.reason;
                if (!validation.valid) {
                    std::cerr << "[sync] WARNING: subtitle file looks broken (" << validation.reason
                              << "): " << ext.file_path << "\n";
                }
                track_list.push_back(std::move(t));
            }
            ++with_ext_subs;
        }

        if (!probe_results[i]) continue; // not re-probed this pass
        ++media_probed;
        const auto& probed = *probe_results[i];

        int64_t new_duration_ms = it.duration_ms;
        if (!durationLooksValid(new_duration_ms)) {
            if (durationLooksValid(probed.duration_ms)) {
                new_duration_ms = probed.duration_ms;
            } else {
                std::cerr << "[sync] WARNING: could not determine valid duration for "
                          << it.mapped_path << " (source=" << it.duration_ms
                          << " ffprobe=" << probed.duration_ms << ")\n";
                new_duration_ms = 0;
            }
        }

        const std::string resolution_label = bucketResolutionLabel(probed.video.height);
        const auto fp = statFingerprint(it.mapped_path);
        auto& updates = it.media_type == "movie" ? movie_updates : episode_updates;
        updates.push_back(ProbeUpdate{
            it.kairos_id,
            resolution_label,
			nlohmann::json(probed.langs.audio).dump(),
			nlohmann::json(probed.langs.subtitle).dump(),
            new_duration_ms,
            nlohmann::json(probed.keyframes_ms).dump(),
            fp.size,
            fp.mtime,
        });

        std::cout << "[sync-advanced]   probed " << media_probed << "/" << to_probe << ": "
                  << it.mapped_path << " (" << resolution_label
				  << ", " << probed.langs.audio.size() << " audio track(s)"
				  << ", " << probed.keyframes_ms.size() << " keyframe(s)"
                  << (has_ext_subs_this_item ? ", external subtitles found" : "")
                  << ")" << std::endl;
    }

    // ── Write (two batched transactions instead of thousands of tiny ones) ────
	// Previously this issued one autocommit UPDATE per file on sync_db_ plus
	// one delete+insert transaction per file on the primary connection — on a
    // large library, thousands of individual write-lock acquisitions ping-
    // ponging between two connections, which made "database is locked" far
    // more likely under any concurrent write (e.g. an API-triggered scraper
    // refresh) and made this pass slow in its own right.
    SubtitleTrackRepository sub_repo(db_);
    if (!episode_ids.empty())
        sub_repo.syncSubtitleTracksBatch("episode", episode_ids, "file", std::move(episode_tracks));
    if (!movie_ids.empty())
        sub_repo.syncSubtitleTracksBatch("movie", movie_ids, "file", std::move(movie_tracks));

    {
        SQLite::Transaction probe_txn(sync_db_, SQLite::TransactionBehavior::IMMEDIATE);
        if (!episode_updates.empty()) {
            SQLite::Statement upd(sync_db_,
                "UPDATE episode SET duration_ms = ?, resolution_label = ?, "
                "audio_languages = ?, embedded_subtitle_languages = ?, "
                "keyframes_ms = ?, keyframes_size = ?, keyframes_mtime = ? WHERE episode_id = ?");
            for (auto& u : episode_updates) {
                upd.bind(1, u.duration_ms);
                upd.bind(2, u.resolution_label);
				upd.bind(3, u.audio_languages);
				upd.bind(4, u.embedded_subs);
				upd.bind(5, u.keyframes_ms);
				upd.bind(6, u.keyframes_size);
                upd.bind(7, u.keyframes_mtime);
                upd.bind(8, u.kairos_id);
                upd.exec();
                upd.reset();
            }
        }
        if (!movie_updates.empty()) {
            SQLite::Statement upd(sync_db_,
								  "UPDATE movie SET duration_ms = ?, resolution_label = ?, "
								  "audio_languages = ?, embedded_subtitle_languages = ?, "
								  "keyframes_ms = ?, keyframes_size = ?, keyframes_mtime = ? WHERE movie_id = ?");
            for (auto& u : movie_updates) {
                upd.bind(1, u.duration_ms);
                upd.bind(2, u.resolution_label);
                upd.bind(3, u.audio_languages);
				upd.bind(4, u.embedded_subs);
				upd.bind(5, u.keyframes_ms);
				upd.bind(6, u.keyframes_size);
				upd.bind(7, u.keyframes_mtime);
				upd.bind(8, u.kairos_id);
                upd.exec();
                upd.reset();
            }
        }
        probe_txn.commit();
    }

    std::cout << "[sync] media probe done: " << source_id
              << " (" << media_probed << "/" << items.size() << " file(s) ffprobed, "
			  << with_ext_subs << " with external subtitles, "
			  << elapsedMs(t_start, std::chrono::steady_clock::now()) << "ms)" << std::endl;
}
