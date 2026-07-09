#include "ScraperManager.h"
#include "AnidbScraper.h"
#include "TmdbScraper.h"
#include "TvdbScraper.h"
#include "conf/ConfStore.h"
#include "db/ContentRepository.h"
#include "db/Database.h"
#include "db/SourceRepository.h"
#include "log/DebugLog.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

namespace fs = std::filesystem;

using json = nlohmann::json;

// ── Title normalisation + similarity ─────────────────────────────────────────

namespace {

std::string normalizeTitle(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto* art : { "the ", "a ", "an " }) {
        if (r.starts_with(art)) { r = r.substr(std::strlen(art)); break; }
    }
    std::string out;
    for (char c : r) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') out += c;
    }
    return out;
}

// Levenshtein similarity [0,1]
double titleSimilarity(const std::string& a, const std::string& b) {
    std::string na = normalizeTitle(a), nb = normalizeTitle(b);
    if (na == nb) return 1.0;
    if (na.empty() || nb.empty()) return 0.0;

    const size_t m = na.size(), n = nb.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            int cost = (na[i - 1] == nb[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({ dp[i-1][j]+1, dp[i][j-1]+1, dp[i-1][j-1]+cost });
        }
    }
    int dist = dp[m][n];
    int maxLen = static_cast<int>(std::max(m, n));
    return 1.0 - static_cast<double>(dist) / maxLen;
}

double computeScore(const std::string& source_title, int source_year,
                    const std::string& cand_title,  int cand_year) {
    double ts = titleSimilarity(source_title, cand_title);
    double yb = 0.0;
    if (source_year > 0 && cand_year > 0) {
        if (source_year == cand_year) yb = 1.0;
        else if (std::abs(source_year - cand_year) <= 1) yb = 0.4;
    } else {
        yb = 0.3; // unknown year on one side — partial credit
    }
    return ts * 0.75 + yb * 0.25;
}

std::string candidateKey(const std::string& item_type, const std::string& kairos_id,
                          const std::string& source,    const std::string& external_id) {
    return item_type + ":" + kairos_id + ":" + source + ":" + external_id;
}

// True when 2+ distinct external ids score within epsilon of the top score —
// a genuine tie the scorer can't break (e.g. two different real movies that
// happen to share an exact title, with the source file's year unknown so
// nothing separates them). computeScore alone can't express "I don't know" —
// an exact title match alone already scores 0.75/1.0 regardless of whether
// there's one candidate with that title or five, so a tie here is real
// ambiguity, not noise, and auto-accepting one anyway means silently picking
// whichever the source API happened to list first.
//
// Exception: if the library has a scraper priority order and one of the
// near-best candidates comes from a source on that list, that's not
// ambiguity — it's a tie we already know how to break, so it doesn't need
// to wait for a human.
template <typename Cand>
bool isAmbiguousTie(const std::vector<Cand>& candidates, double best,
                     const std::vector<std::string>& priority_order, double epsilon = 0.01) {
    std::set<std::string> near_best_ids;
    bool priority_near_best = false;
    for (const auto& c : candidates) {
        if (best - c.score > epsilon) continue;
        near_best_ids.insert(c.ext_id);
        if (std::find(priority_order.begin(), priority_order.end(), c.source) != priority_order.end())
            priority_near_best = true;
    }
    if (priority_near_best) return false;
    return near_best_ids.size() > 1;
}

// Picks the candidate to auto-accept. Ordinarily just the top-scoring one,
// but when the library has a scraper priority order, the near-best candidate
// whose source ranks earliest in that order wins even if a lower-ranked (or
// unranked) source scored marginally higher — the library owner already told
// us which sources to trust and in what order, so a noise-level score
// difference shouldn't override that.
template <typename Cand>
const Cand& pickBest(const std::vector<Cand>& candidates, double best,
                      const std::vector<std::string>& priority_order, double epsilon = 0.01) {
    const Cand* best_ranked = nullptr;
    size_t best_rank = priority_order.size();
    for (const auto& c : candidates) {
        if (best - c.score > epsilon) continue;
        auto it = std::find(priority_order.begin(), priority_order.end(), c.source);
        if (it == priority_order.end()) continue;
        size_t rank = static_cast<size_t>(it - priority_order.begin());
        if (rank < best_rank || (rank == best_rank && (!best_ranked || c.score > best_ranked->score))) {
            best_rank   = rank;
            best_ranked = &c;
        }
    }
    if (best_ranked) return *best_ranked;
    return *std::max_element(candidates.begin(), candidates.end(),
        [](const Cand& a, const Cand& b) { return a.score < b.score; });
}

// Returns true if the mapped path's parent directory exists on disk.
// Returns true (don't block) when the path is empty — nothing to check.
bool folderExists(const std::string& mapped_path) {
    if (mapped_path.empty()) return true;
    try { return fs::exists(fs::path(mapped_path).parent_path()); }
    catch (...) { return true; } // can't stat — assume ok, don't penalise
}

// Strips trailing " (YYYY)" from folder names like "Fargo (2014)".
std::string stripFolderYear(const std::string& name) {
    if (name.size() >= 7 && name.back() == ')') {
        auto pos = name.rfind(" (");
        if (pos != std::string::npos && name.size() - pos == 7) {
            std::string yr = name.substr(pos + 2, 4);
            if (std::all_of(yr.begin(), yr.end(),
                            [](char c){ return std::isdigit(static_cast<unsigned char>(c)); }))
                return name.substr(0, pos);
        }
    }
    return name;
}

// Returns the show-level folder name from an episode path.
// For .../Show Name/Season 1/ep.mkv  → "Show Name"
// For .../Show Name/ep.mkv           → "Show Name"
std::string extractShowFolder(const fs::path& p) {
    fs::path parent = p.parent_path();
    std::string pname = parent.filename().string();
    // Detect season-style folders so we step up one more level.
    std::string lower;
    lower.reserve(pname.size());
    for (char c : pname)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool is_season = lower.starts_with("season") || lower.starts_with("specials")
                     || lower.starts_with("extras") || lower.starts_with("bonus")
                     || (lower.size() >= 2 && lower[0] == 's'
                         && std::isdigit(static_cast<unsigned char>(lower[1])));
    return is_season ? parent.parent_path().filename().string() : pname;
}

// Minimum folder/title similarity to trust a source-supplied external ID.
// Near-identical names only: a mismatch here means the source metadata is
// likely wrong and we should re-scrape using the folder name instead.
constexpr double kFolderTitleThreshold = 0.95;

} // namespace

// ── Constructor ───────────────────────────────────────────────────────────────

ScraperManager::ScraperManager(Database& db, ConfStore& conf)
    : db_(db), conf_(conf)
{
    buildScrapers();
}

ScraperManager::~ScraperManager() = default;

SourceRepository& ScraperManager::sourceRepo() const {
    static SourceRepository repo(db_);
    return repo;
}

// ── Settings ──────────────────────────────────────────────────────────────────

static std::string configKey(const std::string& source, const std::string& field) {
    return "scraper_" + source + "_" + field;
}

ScraperSettings ScraperManager::getSettings() const {
    auto readKey = [&](const std::string& key, const std::string& def) -> std::string {
        SQLite::Statement q(db_.get(),
            "SELECT value FROM app_config WHERE key = ?");
        q.bind(1, key);
        return q.executeStep() ? q.getColumn(0).getString() : def;
    };

    ScraperSettings s;
    s.match_threshold = std::stod(readKey("match_threshold", "0.8"));

    for (const auto* src : { "tmdb", "tvdb", "anidb" }) {
        ScraperConfig c;
        c.source   = src;
        c.api_key  = readKey(configKey(src, "api_key"),  "");
        c.language = readKey(configKey(src, "language"), src == std::string("tvdb") ? "eng" : "en");
        c.enabled  = readKey(configKey(src, "enabled"),  "0") == "1";
        c.language_weight = std::stod(readKey(configKey(src, "language_weight"), "0.1"));
        if (src == std::string("tvdb"))
            c.pin = readKey(configKey(src, "pin"), "");
        s.configs.push_back(c);
    }
    return s;
}

void ScraperManager::updateSettings(const ScraperSettings& s) {
    auto writeKey = [&](const std::string& key, const std::string& val) {
        SQLite::Statement q(db_.get(),
            "INSERT INTO app_config(key,value) VALUES(?,?)"
            " ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        q.bind(1, key); q.bind(2, val); q.exec();
    };

    writeKey("match_threshold", std::to_string(s.match_threshold));
    for (const auto& c : s.configs) {
        writeKey(configKey(c.source, "api_key"),  c.api_key);
        writeKey(configKey(c.source, "language"), c.language);
        writeKey(configKey(c.source, "enabled"),  c.enabled ? "1" : "0");
        writeKey(configKey(c.source, "language_weight"), std::to_string(c.language_weight));
        if (c.source == "tvdb") writeKey(configKey(c.source, "pin"), c.pin);
    }
    buildScrapers();
}

std::vector<ScraperManager::ExternalId> ScraperManager::getExternalIds(const std::string& kairos_id, const std::string& item_type) const {
    std::vector<ExternalId> out;
    SQLite::Statement q(db_.get(),
        "SELECT source, external_id, priority FROM item_external_id WHERE kairos_id = ? AND item_type = ? ORDER BY priority ASC");
    q.bind(1, kairos_id);
    q.bind(2, item_type);
    while (q.executeStep()) {
        out.push_back({
            q.getColumn(0).getString(),
            q.getColumn(1).getString(),
            q.getColumn(2).getInt()
        });
    }
    return out;
}

void ScraperManager::setExternalIds(const std::string& kairos_id, const std::string& item_type, const std::vector<ExternalId>& ids) {
    SQLite::Transaction txn(db_.get());
    {
        SQLite::Statement d(db_.get(), "DELETE FROM item_external_id WHERE kairos_id = ? AND item_type = ?");
        d.bind(1, kairos_id);
        d.bind(2, item_type);
        d.exec();
    }
    SQLite::Statement ins(db_.get(),
        "INSERT INTO item_external_id (item_type, kairos_id, source, external_id, priority) VALUES (?, ?, ?, ?, ?)");
    for (const auto& id : ids) {
        ins.bind(1, item_type);
        ins.bind(2, kairos_id);
        ins.bind(3, id.source);
        ins.bind(4, id.external_id);
        ins.bind(5, id.priority);
        ins.exec();
        ins.reset();
    }
    txn.commit();
}

std::vector<std::string> ScraperManager::getAlternateTitles(const std::string& kairos_id, const std::string& item_type) const {
    std::vector<std::string> out;
    SQLite::Statement q(db_.get(),
        "SELECT title FROM item_alternate_title WHERE kairos_id = ? AND item_type = ?");
    q.bind(1, kairos_id);
    q.bind(2, item_type);
    while (q.executeStep()) {
        out.push_back(q.getColumn(0).getString());
    }
    return out;
}

void ScraperManager::setAlternateTitles(const std::string& kairos_id, const std::string& item_type, const std::vector<std::string>& titles) {
    SQLite::Transaction txn(db_.get());
    {
        SQLite::Statement d(db_.get(), "DELETE FROM item_alternate_title WHERE kairos_id = ? AND item_type = ?");
        d.bind(1, kairos_id);
        d.bind(2, item_type);
        d.exec();
    }
    SQLite::Statement ins(db_.get(),
        "INSERT INTO item_alternate_title (item_type, kairos_id, title) VALUES (?, ?, ?)");
    for (const auto& title : titles) {
        if (title.empty()) continue;
        ins.bind(1, item_type);
        ins.bind(2, kairos_id);
        ins.bind(3, title);
        ins.exec();
        ins.reset();
    }
    txn.commit();
}

void ScraperManager::upsertExternalId(const std::string& item_type, const std::string& kairos_id,
                                       const std::string& source,    const std::string& external_id,
                                       int priority) {
    if (external_id.empty()) return;
    SQLite::Statement q(db_.get(),
        "INSERT INTO item_external_id(item_type, kairos_id, source, external_id, priority)"
        " VALUES(?, ?, ?, ?, ?)"
        " ON CONFLICT(item_type, kairos_id, source) DO UPDATE SET external_id=excluded.external_id, priority=excluded.priority");
    q.bind(1, item_type);
    q.bind(2, kairos_id);
    q.bind(3, source);
    q.bind(4, external_id);
    q.bind(5, priority);
    q.exec();
}

// Links (source, external_id) to this item without ever silently discarding
// another already-linked source's entry.
//
// promote_to_primary=true means a human explicitly chose this as *the*
// match (queue accept, manual "Fix Match" pick) — it becomes priority 1 and
// every other id shifts down to make room, but none of them are deleted.
// Before this existed, accepting a candidate from a source other than the
// current primary just upserted priority=1 onto that one source's row,
// leaving the old primary's row still at priority 1 too — two rows tied for
// "the" primary source, with the tie broken only by incidental row order.
//
// promote_to_primary=false is for ids a scraper's own API response
// cross-references (e.g. TMDB's show payload also naming a TVDB id) — it
// only fills in a source that has no entry yet, appended after the current
// max priority. If that source is already linked, this is a no-op: an
// automatic cross-reference must never overwrite something a human already
// set for that source.
void ScraperManager::linkExternalId(const std::string& item_type, const std::string& kairos_id,
                                     const std::string& source,    const std::string& external_id,
                                     bool promote_to_primary) {
    if (external_id.empty()) return;
    auto ids = getExternalIds(kairos_id, item_type); // priority ASC
    bool has_source = std::any_of(ids.begin(), ids.end(),
        [&](const ExternalId& e) { return e.source == source; });

    if (!promote_to_primary) {
        if (has_source) return;
        int next_priority = 1;
        for (const auto& e : ids) next_priority = std::max(next_priority, e.priority + 1);
        upsertExternalId(item_type, kairos_id, source, external_id, next_priority);
        return;
    }

    std::vector<ExternalId> reordered;
    reordered.push_back({source, external_id, 1});
    int next = 2;
    for (const auto& e : ids) {
        if (e.source == source) continue;
        reordered.push_back({e.source, e.external_id, next++});
    }
    setExternalIds(kairos_id, item_type, reordered);
}

void ScraperManager::upsertAlternateTitle(const std::string& item_type, const std::string& kairos_id,
                                           const std::string& title) {
    if (title.empty()) return;
    SQLite::Statement q(db_.get(),
        "INSERT OR IGNORE INTO item_alternate_title (item_type, kairos_id, title) VALUES (?, ?, ?)");
    q.bind(1, item_type);
    q.bind(2, kairos_id);
    q.bind(3, title);
    q.exec();
}

void ScraperManager::buildScrapers() {
    auto s = getSettings();
    anidb_.reset();
    tmdb_.reset();
    tvdb_.reset();
    for (const auto& c : s.configs) {
        if (!c.enabled || c.api_key.empty()) continue;
        if (c.source == "tmdb")  tmdb_  = std::make_unique<TmdbScraper>(c.api_key, c.language);
        if (c.source == "tvdb")  tvdb_  = std::make_unique<TvdbScraper>(c.api_key, c.language, c.pin);
        if (c.source == "anidb") anidb_ = std::make_unique<AnidbScraper>(c.api_key);
    }
}

double ScraperManager::threshold() const {
    // Delegates to getSettings() so there's exactly one place that defines
    // the default match_threshold — this used to hardcode its own fallback
    // (1.0) that had drifted out of sync with getSettings()'s (0.8), so a
    // fresh app_config with no match_threshold row silently required a
    // perfect score to auto-accept while Settings displayed 0.8.
    try { return getSettings().match_threshold; }
    catch (...) { return 0.8; }
}

// ── Trigger / background match ────────────────────────────────────────────────

void ScraperManager::triggerMatch(const std::string& target_id,
                                   const std::string& item_type) {
    bool expected = false;
    if (!matching_.compare_exchange_strong(expected, true)) {
        std::cout << "[scraper] match already running — ignoring trigger\n";
        return;
    }
    std::thread([this, target_id, item_type]() {
        try { runMatch(target_id, item_type); }
        catch (const std::exception& e) {
            std::cerr << "[scraper] match error: " << e.what() << "\n";
        }
        matching_.store(false);
    }).detach();
}

void ScraperManager::runMatchSync(const std::string& target_id,
                                   const std::string& item_type) {
    bool expected = false;
    if (!matching_.compare_exchange_strong(expected, true)) {
        std::cout << "[scraper] match already running — skipping inline match\n";
        return;
    }
    try { runMatch(target_id, item_type); }
    catch (const std::exception& e) {
        std::cerr << "[scraper] match error: " << e.what() << "\n";
    }
    matching_.store(false);
}

void ScraperManager::runMatch(const std::string& target_id,
                               const std::string& item_type) {
    // Count pending items so the log line gives useful context up front.
    // Same LEFT JOIN shape as the selection queries below, so a library-level
    // skip_scraping exemption is reflected in both places consistently.
    int pending_shows = 0, pending_movies = 0;
    if (item_type.empty() || item_type == "show") {
        std::string csql = R"(
            SELECT COUNT(*) FROM show s
            LEFT JOIN source_mapping sm ON sm.kairos_id = s.show_id AND sm.item_type = 'show'
            LEFT JOIN media_library ml ON ml.library_id = sm.library_id
            WHERE s.match_status IN ('unscraped','uncertain','unmatched')
              AND s.skip_scraping = 0 AND COALESCE(ml.skip_scraping, 0) = 0
        )";
        if (!target_id.empty()) csql += " AND s.show_id='" + target_id + "'";
        SQLite::Statement cq(db_.get(), csql);
        if (cq.executeStep()) pending_shows = cq.getColumn(0).getInt();
    }
    if (item_type.empty() || item_type == "movie") {
        std::string csql = R"(
            SELECT COUNT(*) FROM movie m
            LEFT JOIN source_mapping sm ON sm.kairos_id = m.movie_id AND sm.item_type = 'movie'
            LEFT JOIN media_library ml ON ml.library_id = sm.library_id
            WHERE m.match_status IN ('unscraped','uncertain','unmatched')
              AND m.skip_scraping = 0 AND COALESCE(ml.skip_scraping, 0) = 0
        )";
        if (!target_id.empty()) csql += " AND m.movie_id='" + target_id + "'";
        SQLite::Statement cq(db_.get(), csql);
        if (cq.executeStep()) pending_movies = cq.getColumn(0).getInt();
    }

    if (pending_shows + pending_movies == 0) {
        std::cout << "[scraper] match: nothing pending\n";
        return;
    }
    std::cout << "[scraper] match: " << pending_shows << " show(s), "
              << pending_movies << " movie(s)\n";
    const auto t_match_start = std::chrono::steady_clock::now();

    // ── Shows ────────────────────────────────────────────────────────────────
    if (item_type.empty() || item_type == "show") {
        std::string sql = R"(
            SELECT s.show_id, s.title, COALESCE(s.year,0), s.tmdb_id, s.tvdb_id,
              COALESCE(ml.library_id, '') AS library_id,
              COALESCE(ml.include_anidb, 0) AS include_anidb
            FROM show s
            LEFT JOIN source_mapping sm ON sm.kairos_id = s.show_id AND sm.item_type = 'show'
            LEFT JOIN media_library ml ON ml.library_id = sm.library_id
            WHERE s.match_status IN ('unscraped','uncertain','unmatched')
              AND s.skip_scraping = 0 AND COALESCE(ml.skip_scraping, 0) = 0
        )";
        if (!target_id.empty()) sql += " AND s.show_id = '" + target_id + "'";

        SQLite::Statement q(db_.get(), sql);
        while (q.executeStep()) {
            // matchShow() itself folds in any alternate titles for kid into a
            // single search/score/accept decision — it must only be called
            // once per item per pass (see matchShow's own comment for why).
            matchShow(q.getColumn(5).getString(),  // library_id
                      q.getColumn(0).getString(),  // kairos_id
                      q.getColumn(1).getString(),  // title
                      q.getColumn(2).getInt(),     // year
                      q.getColumn(3).getString(),  // tmdb_id
                      q.getColumn(4).getString(),  // tvdb_id
                      q.getColumn(6).getInt() != 0); // include_anidb
        }
    }

    // ── Movies ───────────────────────────────────────────────────────────────
    if (item_type.empty() || item_type == "movie") {
        std::string sql = R"(
            SELECT m.movie_id, m.title, COALESCE(m.year,0), m.tmdb_id, COALESCE(m.file_path,''),
              COALESCE(ml.library_id, '') AS library_id,
              COALESCE(ml.include_anidb, 0) AS include_anidb
            FROM movie m
            LEFT JOIN source_mapping sm ON sm.kairos_id = m.movie_id AND sm.item_type = 'movie'
            LEFT JOIN media_library ml ON ml.library_id = sm.library_id
            WHERE m.match_status IN ('unscraped','uncertain','unmatched')
              AND m.skip_scraping = 0 AND COALESCE(ml.skip_scraping, 0) = 0
        )";
        if (!target_id.empty()) sql += " AND m.movie_id = '" + target_id + "'";

        SQLite::Statement q(db_.get(), sql);
        while (q.executeStep()) {
            // matchMovie() itself folds in any alternate titles for kid into
            // a single search/score/accept decision — it must only be called
            // once per item per pass (see matchMovie's own comment for why).
            matchMovie(q.getColumn(5).getString(),  // library_id
                       q.getColumn(0).getString(),  // kairos_id
                       q.getColumn(1).getString(),  // title
                       q.getColumn(2).getInt(),     // year
                       q.getColumn(3).getString(),  // tmdb_id
                       q.getColumn(4).getString(),  // file_path
                       q.getColumn(6).getInt() != 0); // include_anidb
        }
    }
    std::cout << "[scraper] match complete ("
              << elapsedMs(t_match_start, std::chrono::steady_clock::now()) << "ms)\n";
}

// ── Per-item matching ─────────────────────────────────────────────────────────

void ScraperManager::matchShow(const std::string& library_id, const std::string& kairos_id, const std::string& title,
                                int year, const std::string& tmdb_id,
                                const std::string& tvdb_id,
                                bool include_anidb) {
    // Resolve the effective search title. When the on-disk folder name disagrees
    // with the source-provided title, the folder is more trustworthy.
    std::string search_title = title;
    bool has_paths = false, path_ok = false;
    {
        SQLite::Statement ep(db_.get(),
            "SELECT file_path FROM episode WHERE show_id = ? AND file_path != '' LIMIT 5");
        ep.bind(1, kairos_id);
        while (ep.executeStep() && !path_ok) {
            has_paths = true;
            std::string mapped = conf_.applyPathMap(ep.getColumn(0).getString());
            if (!folderExists(mapped)) continue;
            path_ok = true;
            std::string folder  = extractShowFolder(fs::path(mapped));
            std::string stripped = stripFolderYear(folder);
            double sim = titleSimilarity(title, stripped);
            if (sim < kFolderTitleThreshold && !stripped.empty()) {
                search_title = stripped;
                std::cout << "[scraper]   \"" << title << "\" folder mismatch"
                          << " (folder: \"" << folder << "\", sim="
                          << std::fixed << std::setprecision(2) << sim
                          << ") — searching as \"" << search_title << "\"\n";
            }
        }
    }

    // Items with existing external IDs are trusted only when the folder name
    // agrees with the source title. A mismatch falls through to a fresh search.
    if (!tmdb_id.empty() || !tvdb_id.empty()) {
        if (has_paths && !path_ok) {
            std::cout << "[scraper]   \"" << title << "\" → uncertain (path missing)\n";
            std::string src = !tmdb_id.empty() ? "tmdb" : "tvdb";
            std::string eid = !tmdb_id.empty() ? tmdb_id : tvdb_id;
            storeCandidate("show", kairos_id, src, eid, title, year, 1.0, "", "");
            setMatchStatus("show", kairos_id, "uncertain", 1.0);
            return;
        }
        if (search_title == title) {
            std::cout << "[scraper]   \"" << title << "\" → matched\n";
            setMatchStatus("show", kairos_id, "matched", 1.0);
            return;
        }
        // Folder mismatch: ignore the provided ID and search with the folder name.
    }

    if (search_title.empty()) {
        std::cout << "[scraper]   (no title, id=" << kairos_id << ") → unmatched\n";
        setMatchStatus("show", kairos_id, "unmatched", 0.0);
        return;
    }

    std::string pref_lang;
    if (auto lib = sourceRepo().getLibrary(library_id)) {
        pref_lang = lib->preferred_language;
    }
    std::vector<std::string> priority = sourceRepo().getScraperPriority(library_id, "show");

    // Every enabled source is queried once per candidate title — the
    // folder/source-resolved title plus any admin-supplied alternate titles —
    // so the item gets exactly ONE scoring/accept decision per matchShow()
    // call. This used to be the caller's job (runMatch() invoked matchShow()
    // once per alternate title), which meant each alt-title call independently
    // overwrote match_status/match_score and could auto-accept a *different*
    // candidate without clearing the previous call's accepted flag — a noisy
    // alternate title searched after the real title could silently downgrade
    // (or double-accept) an otherwise-clean match within the same run.
    std::vector<std::string> query_titles = { search_title };
    for (const auto& alt : getAlternateTitles(kairos_id, "show")) {
        if (!alt.empty() && std::find(query_titles.begin(), query_titles.end(), alt) == query_titles.end())
            query_titles.push_back(alt);
    }

    struct Cand { std::string source, ext_id, cand_title, poster, overview; int cand_year; double score; };
    std::vector<Cand> candidates;

    // Merges a search result into `candidates`, keyed by (source, ext_id), so
    // the same external show turning up under two different query titles
    // collapses into one candidate that keeps the higher of the two scores
    // instead of one search's result silently clobbering the other's.
    auto collect = [&](const std::string& source, const std::string& query_title, std::vector<Show> results) {
        double lang_bonus = 0.0;
        for (const auto& cfg : getSettings().configs) {
            if (cfg.source == source && !pref_lang.empty() && cfg.language == pref_lang) {
                lang_bonus = cfg.language_weight;
                break;
            }
        }

        for (auto& r : results) {
            double sc = computeScore(query_title, year, r.title,
                                     r.year.has_value() ? r.year.value() : 0);
            sc += lang_bonus;
            std::string ext;
            if (source == "tmdb")       ext = r.tmdb_id;
            else if (source == "tvdb")  ext = r.tvdb_id;
            else if (source == "anidb") ext = r.show_id;  // AID stored in show_id
            if (ext.empty()) continue;

            auto it = std::find_if(candidates.begin(), candidates.end(),
                [&](const Cand& c) { return c.source == source && c.ext_id == ext; });
            if (it != candidates.end()) {
                if (sc > it->score) *it = { source, ext, r.title, r.thumb, r.overview,
                                            r.year.has_value() ? r.year.value() : 0, sc };
            } else {
                candidates.push_back({ source, ext, r.title, r.thumb, r.overview,
                                       r.year.has_value() ? r.year.value() : 0, sc });
            }
        }
    };

    auto timedSearch = [&](const std::string& src, const std::string& query_title, std::vector<Show> results) {
        DLOG << "[scraper]   " << src << " (\"" << query_title << "\"): " << results.size() << " result(s)\n";
        for (const auto& r : results)
            DLOG << "[scraper]     " << src << " candidate: \"" << r.title << "\""
                 << " (" << (r.year.has_value() ? r.year.value() : 0) << ")"
                 << " score=" << std::fixed << std::setprecision(3)
                 << computeScore(query_title, year, r.title, r.year.has_value() ? r.year.value() : 0)
                 << '\n';
        collect(src, query_title, std::move(results));
    };
    // A library's chosen scraper is a *preference*, not an exclusive filter —
    // every enabled scraper is still queried so a title missing from the
    // preferred source can still be found elsewhere; the preference instead
    // breaks ties in the scoring below (see isAmbiguousTie/pickBest).
    for (const auto& qt : query_titles) {
        if (tmdb_) {
            DLOG << "[scraper]   querying tmdb for show \"" << qt << "\" year=" << year << '\n';
            const auto t0 = std::chrono::steady_clock::now();
            auto results = tmdb_->searchShows(qt, year);
            // TMDB/TVDB treat `year` as a hard filter — a locally-parsed year that's
            // off by one (or just wrong) silently zeroes the whole result set even
            // when the title matches perfectly. Retry unfiltered rather than let a
            // bad year turn a real match into "unmatched"; computeScore() already
            // gives an unknown/mismatched year only partial credit, so this can't
            // cause a wrong auto-accept, only a candidate worth reviewing.
            if (results.empty() && year > 0) {
                DLOG << "[scraper]   tmdb: 0 results with year filter, retrying without year\n";
                results = tmdb_->searchShows(qt, 0);
            }
            DLOG << "[scraper]   tmdb done in " << elapsedMs(t0, std::chrono::steady_clock::now()) << "ms\n";
            timedSearch("tmdb", qt, std::move(results));
        }
        if (tvdb_) {
            DLOG << "[scraper]   querying tvdb for show \"" << qt << "\" year=" << year << '\n';
            const auto t0 = std::chrono::steady_clock::now();
            auto results = tvdb_->searchShows(qt, year);
            if (results.empty() && year > 0) {
                DLOG << "[scraper]   tvdb: 0 results with year filter, retrying without year\n";
                results = tvdb_->searchShows(qt, 0);
            }
            DLOG << "[scraper]   tvdb done in " << elapsedMs(t0, std::chrono::steady_clock::now()) << "ms\n";
            timedSearch("tvdb", qt, std::move(results));
        }
        // AniDB is anime-only — never queried for a library unless the admin
        // explicitly opted it in (include_anidb) or placed it anywhere in
        // this library's scraper priority order. Prevents cross-matching an
        // unrelated general movie/show library against anime titles on
        // title text alone.
        if (anidb_ && (include_anidb || std::find(priority.begin(), priority.end(), "anidb") != priority.end())) {
            DLOG << "[scraper]   querying anidb for show \"" << qt << "\" year=" << year << '\n';
            const auto t0 = std::chrono::steady_clock::now();
            auto results = anidb_->searchShows(qt, year);
            DLOG << "[scraper]   anidb done in " << elapsedMs(t0, std::chrono::steady_clock::now()) << "ms\n";
            timedSearch("anidb", qt, std::move(results));
        }
    }

    double best = 0.0;
    for (const auto& c : candidates) {
        storeCandidate("show", kairos_id, c.source, c.ext_id,
                        c.cand_title, c.cand_year, c.score, c.poster, c.overview);
        if (c.score > best) best = c.score;
    }

    if (candidates.empty()) {
        std::cout << "[scraper]   \"" << search_title << "\" → unmatched\n";
        setMatchStatus("show", kairos_id, "unmatched", 0.0);
    } else if (isAmbiguousTie(candidates, best, priority)) {
        std::cout << "[scraper]   \"" << search_title << "\" → uncertain"
                  << " (ambiguous: 2+ distinct candidates tied at "
                  << std::fixed << std::setprecision(2) << best << ")\n";
        setMatchStatus("show", kairos_id, "uncertain", best);
    } else if (best >= threshold()) {
        const auto& best_c = pickBest(candidates, best, priority);
        std::cout << "[scraper]   \"" << search_title << "\" → matched"
                  << " (" << best_c.source << ", "
                  << std::fixed << std::setprecision(2) << best << ")\n";
        std::string cid = candidateKey("show", kairos_id, best_c.source, best_c.ext_id);
        SQLite::Statement upd(db_.get(),
            "UPDATE item_match_candidate SET accepted=1 WHERE candidate_id=?");
        upd.bind(1, cid); upd.exec();
        setMatchStatus("show", kairos_id, "matched", best);
    } else {
        std::cout << "[scraper]   \"" << search_title << "\" → uncertain"
                  << " (" << std::fixed << std::setprecision(2) << best << ")\n";
        setMatchStatus("show", kairos_id, "uncertain", best);
    }
}

void ScraperManager::matchMovie(const std::string& library_id, const std::string& kairos_id, const std::string& title,
                                 int year, const std::string& tmdb_id,
                                 const std::string& file_path,
                                 bool include_anidb) {
    // Resolve the effective search title. When the on-disk folder name disagrees
    // with the source-provided title, the folder is more trustworthy.
    std::string search_title = title;
    if (!file_path.empty()) {
        std::string mapped = conf_.applyPathMap(file_path);
        if (!folderExists(mapped)) {
            if (!tmdb_id.empty()) {
                std::cout << "[scraper]   \"" << title << "\" → uncertain (path missing)\n";
                storeCandidate("movie", kairos_id, "tmdb", tmdb_id, title, year, 1.0, "", "");
                setMatchStatus("movie", kairos_id, "uncertain", 1.0);
                return;
            }
        } else {
            std::string folder = stripFolderYear(fs::path(mapped).parent_path().filename().string());
            if (!folder.empty()) {
                double sim = titleSimilarity(title, folder);
                if (sim < kFolderTitleThreshold) {
                    search_title = folder;
                    std::cout << "[scraper]   \"" << title << "\" folder mismatch"
                              << " (folder: \"" << folder << "\", sim="
                              << std::fixed << std::setprecision(2) << sim
                              << ") — searching as \"" << search_title << "\"\n";
                }
            }
        }
    }

    if (!tmdb_id.empty() && search_title == title) {
        // Folder matched (or no file path to check) — trust the provided ID.
        std::cout << "[scraper]   \"" << title << "\" → matched\n";
        setMatchStatus("movie", kairos_id, "matched", 1.0);
        return;
    }

    // Search path: no trusted ID, or folder mismatch disqualified the provided ID.
    if (search_title.empty()) {
        std::cout << "[scraper]   (no title, id=" << kairos_id << ") → unmatched\n";
        setMatchStatus("movie", kairos_id, "unmatched", 0.0);
        return;
    }

    std::string pref_lang;
    if (auto lib = sourceRepo().getLibrary(library_id)) {
        pref_lang = lib->preferred_language;
    }
    std::vector<std::string> priority = sourceRepo().getScraperPriority(library_id, "movie");

    // See matchShow() for why this loops over query titles (real + alternate)
    // and merges into one candidate pool instead of the caller invoking this
    // function once per alternate title.
    std::vector<std::string> query_titles = { search_title };
    for (const auto& alt : getAlternateTitles(kairos_id, "movie")) {
        if (!alt.empty() && std::find(query_titles.begin(), query_titles.end(), alt) == query_titles.end())
            query_titles.push_back(alt);
    }

    struct Cand { std::string source, ext_id, cand_title, poster, overview; int cand_year; double score; };
    std::vector<Cand> candidates;

    // Merges by (source, ext_id), keeping the higher score when the same
    // external movie turns up under two different query titles.
    auto collect = [&](const std::string& source, const std::string& query_title, std::vector<Movie> results) {
        double lang_bonus = 0.0;
        for (const auto& cfg : getSettings().configs) {
            if (cfg.source == source && !pref_lang.empty() && cfg.language == pref_lang) {
                lang_bonus = cfg.language_weight;
                break;
            }
        }

        for (auto& r : results) {
            double sc = computeScore(query_title, year, r.title,
                                     r.year.has_value() ? r.year.value() : 0);
            sc += lang_bonus;
            std::string ext;
            if (source == "tmdb")       ext = r.tmdb_id;
            else if (source == "tvdb")  ext = r.imdb_id;
            else if (source == "anidb") ext = r.movie_id;  // AID stored in movie_id
            if (ext.empty()) continue;

            auto it = std::find_if(candidates.begin(), candidates.end(),
                [&](const Cand& c) { return c.source == source && c.ext_id == ext; });
            if (it != candidates.end()) {
                if (sc > it->score) *it = { source, ext, r.title, r.thumb, r.overview,
                                            r.year.has_value() ? r.year.value() : 0, sc };
            } else {
                candidates.push_back({ source, ext, r.title, r.thumb, r.overview,
                                       r.year.has_value() ? r.year.value() : 0, sc });
            }
        }
    };

    auto timedSearchM = [&](const std::string& src, const std::string& query_title, std::vector<Movie> results) {
        DLOG << "[scraper]   " << src << " (\"" << query_title << "\"): " << results.size() << " result(s)\n";
        for (const auto& r : results)
            DLOG << "[scraper]     " << src << " candidate: \"" << r.title << "\""
                 << " (" << (r.year.has_value() ? r.year.value() : 0) << ")"
                 << " score=" << std::fixed << std::setprecision(3)
                 << computeScore(query_title, year, r.title, r.year.has_value() ? r.year.value() : 0)
                 << '\n';
        collect(src, query_title, std::move(results));
    };
    // A library's chosen scraper is a *preference*, not an exclusive filter —
    // every enabled scraper is still queried so a title missing from the
    // preferred source can still be found elsewhere; the preference instead
    // breaks ties in the scoring below (see isAmbiguousTie/pickBest).
    for (const auto& qt : query_titles) {
        if (tmdb_) {
            DLOG << "[scraper]   querying tmdb for movie \"" << qt << "\" year=" << year << '\n';
            const auto t0 = std::chrono::steady_clock::now();
            auto results = tmdb_->searchMovies(qt, year);
            // See matchShow(): year is a hard filter on TMDB/TVDB, so fall back to
            // an unfiltered search rather than lose an otherwise-clean title match.
            if (results.empty() && year > 0) {
                DLOG << "[scraper]   tmdb: 0 results with year filter, retrying without year\n";
                results = tmdb_->searchMovies(qt, 0);
            }
            DLOG << "[scraper]   tmdb done in " << elapsedMs(t0, std::chrono::steady_clock::now()) << "ms\n";
            timedSearchM("tmdb", qt, std::move(results));
        }
        if (tvdb_) {
            DLOG << "[scraper]   querying tvdb for movie \"" << qt << "\" year=" << year << '\n';
            const auto t0 = std::chrono::steady_clock::now();
            auto results = tvdb_->searchMovies(qt, year);
            if (results.empty() && year > 0) {
                DLOG << "[scraper]   tvdb: 0 results with year filter, retrying without year\n";
                results = tvdb_->searchMovies(qt, 0);
            }
            DLOG << "[scraper]   tvdb done in " << elapsedMs(t0, std::chrono::steady_clock::now()) << "ms\n";
            timedSearchM("tvdb", qt, std::move(results));
        }
        // AniDB is anime-only — never queried for a library unless the admin
        // explicitly opted it in (include_anidb) or placed it anywhere in
        // this library's scraper priority order. Prevents cross-matching an
        // unrelated general movie/show library against anime titles on
        // title text alone.
        if (anidb_ && (include_anidb || std::find(priority.begin(), priority.end(), "anidb") != priority.end())) {
            DLOG << "[scraper]   querying anidb for movie \"" << qt << "\" year=" << year << '\n';
            const auto t0 = std::chrono::steady_clock::now();
            auto results = anidb_->searchMovies(qt, year);
            DLOG << "[scraper]   anidb done in " << elapsedMs(t0, std::chrono::steady_clock::now()) << "ms\n";
            timedSearchM("anidb", qt, std::move(results));
        }
    }

    double best = 0.0;
    for (const auto& c : candidates) {
        storeCandidate("movie", kairos_id, c.source, c.ext_id,
                        c.cand_title, c.cand_year, c.score, c.poster, c.overview);
        if (c.score > best) best = c.score;
    }

    if (candidates.empty()) {
        std::cout << "[scraper]   \"" << search_title << "\" → unmatched\n";
        setMatchStatus("movie", kairos_id, "unmatched", 0.0);
    } else if (isAmbiguousTie(candidates, best, priority)) {
        std::cout << "[scraper]   \"" << search_title << "\" → uncertain"
                  << " (ambiguous: 2+ distinct candidates tied at "
                  << std::fixed << std::setprecision(2) << best << ")\n";
        setMatchStatus("movie", kairos_id, "uncertain", best);
    } else if (best >= threshold()) {
        const auto& best_c = pickBest(candidates, best, priority);
        std::cout << "[scraper]   \"" << search_title << "\" → matched"
                  << " (" << best_c.source << ", "
                  << std::fixed << std::setprecision(2) << best << ")\n";
        std::string cid = candidateKey("movie", kairos_id, best_c.source, best_c.ext_id);
        SQLite::Statement upd(db_.get(),
            "UPDATE item_match_candidate SET accepted=1 WHERE candidate_id=?");
        upd.bind(1, cid); upd.exec();
        setMatchStatus("movie", kairos_id, "matched", best);
    } else {
        std::cout << "[scraper]   \"" << search_title << "\" → uncertain"
                  << " (" << std::fixed << std::setprecision(2) << best << ")\n";
        setMatchStatus("movie", kairos_id, "uncertain", best);
    }
}

void ScraperManager::storeCandidate(const std::string& item_type,
                                     const std::string& kairos_id,
                                     const std::string& source,
                                     const std::string& external_id,
                                     const std::string& title, int year, double score,
                                     const std::string& poster_url,
                                     const std::string& overview) {
    std::string cid = candidateKey(item_type, kairos_id, source, external_id);
    SQLite::Statement s(db_.get(), R"(
        INSERT INTO item_match_candidate
            (candidate_id, item_type, kairos_id, source, external_id,
             title, year, score, poster_url, overview)
        VALUES (?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(item_type, kairos_id, source, external_id) DO UPDATE SET
            score      = excluded.score,
            title      = excluded.title,
            year       = excluded.year,
            poster_url = excluded.poster_url,
            overview   = excluded.overview
    )");
    s.bind(1, cid); s.bind(2, item_type); s.bind(3, kairos_id);
    s.bind(4, source); s.bind(5, external_id);
    s.bind(6, title);
    if (year > 0) s.bind(7, year); else s.bind(7);
    s.bind(8, score);
    s.bind(9, poster_url); s.bind(10, overview);
    s.exec();
}

void ScraperManager::setMatchStatus(const std::string& item_type,
                                     const std::string& kairos_id,
                                     const std::string& status, double score) {
    std::string sql = "UPDATE " + item_type
        + " SET match_status=?, match_score=? WHERE " + item_type + "_id=?";
    SQLite::Statement q(db_.get(), sql);
    q.bind(1, status); q.bind(2, score); q.bind(3, kairos_id);
    q.exec();
}

// ── Accept / reject ───────────────────────────────────────────────────────────

bool ScraperManager::acceptCandidate(const std::string& candidate_id) {
    // Read candidate
    SQLite::Statement rd(db_.get(),
        "SELECT item_type, kairos_id, source, external_id, score "
        "FROM item_match_candidate WHERE candidate_id=?");
    rd.bind(1, candidate_id);
    if (!rd.executeStep()) return false;

    std::string item_type   = rd.getColumn(0).getString();
    std::string kairos_id   = rd.getColumn(1).getString();
    std::string source      = rd.getColumn(2).getString();
    std::string external_id = rd.getColumn(3).getString();
    double score            = rd.getColumn(4).getDouble();

    SQLite::Transaction txn(db_.get());

    // Mark accepted; reject all others for this item
    SQLite::Statement upd(db_.get(),
        "UPDATE item_match_candidate SET accepted = "
        "  CASE WHEN candidate_id=? THEN 1 ELSE 0 END "
        "WHERE item_type=? AND kairos_id=? AND accepted IS NULL");
    upd.bind(1, candidate_id); upd.bind(2, item_type); upd.bind(3, kairos_id);
    upd.exec();

    setMatchStatus(item_type, kairos_id, "matched", score);

    // acceptCandidate() is only ever reached via a human action (the review
    // queue's accept route, or a manual match search) — never by the
    // threshold-clearing auto-match branch above in this file. This is the
    // one place match_confirmed gets set, and it's what gates metadata
    // writeback to Plex/Jellyfin (see architecture.md / writeback plan).
    {
        SQLite::Statement conf(db_.get(),
            "UPDATE " + item_type + " SET match_confirmed = 1 WHERE " + item_type + "_id = ?");
        conf.bind(1, kairos_id);
        conf.exec();
    }

    txn.commit();
    std::cout << "[scraper] match confirmed for " << item_type << " " << kairos_id << " (cid=" << candidate_id << ")\n";

    // Resolve effective metadata language: item-level override → library default → "".
    // Empty string tells each scraper to use its own configured default.
    std::string language;
    try {
        const std::string lang_sql = item_type == "show" ? R"(
            SELECT COALESCE(NULLIF(s.preferred_language,''),
                            NULLIF(ml.preferred_language,''), '')
            FROM show s
            LEFT JOIN source_mapping sm ON sm.kairos_id = s.show_id AND sm.item_type = 'show'
            LEFT JOIN media_library ml ON ml.library_id = sm.library_id
            WHERE s.show_id = ? LIMIT 1
        )" : R"(
            SELECT COALESCE(NULLIF(m.preferred_language,''),
                            NULLIF(ml.preferred_language,''), '')
            FROM movie m
            LEFT JOIN source_mapping sm ON sm.kairos_id = m.movie_id AND sm.item_type = 'movie'
            LEFT JOIN media_library ml ON ml.library_id = sm.library_id
            WHERE m.movie_id = ? LIMIT 1
        )";
        SQLite::Statement lq(db_.get(), lang_sql);
        lq.bind(1, kairos_id);
        if (lq.executeStep()) language = lq.getColumn(0).getString();
    } catch (const std::exception& e) {
        std::cerr << "[scraper] language resolution failed for " << kairos_id << ": " << e.what() << "\n";
    }

    // Best-effort: fetch and apply full metadata from the scraper
    try {
        linkExternalId(item_type, kairos_id, source, external_id, /*promote_to_primary=*/true);
        if (source == "anidb" && anidb_) {
            if (item_type == "show") {
                auto show = anidb_->fetchShow(external_id, language);
                if (show) {
                    linkExternalId(item_type, kairos_id, "tvdb", show->tvdb_id, /*promote_to_primary=*/false);
                    SQLite::Statement app(db_.get(), R"(
                        UPDATE show SET
                            tvdb_id       = CASE WHEN locked THEN tvdb_id       ELSE ? END,
                            overview      = CASE WHEN locked THEN overview      ELSE ? END,
                            status        = CASE WHEN locked THEN status        ELSE ? END,
                            thumb         = CASE WHEN locked THEN thumb         ELSE ? END,
                            art           = CASE WHEN locked THEN art           ELSE ? END,
                            content_rating= CASE WHEN locked THEN content_rating ELSE ? END
                        WHERE show_id = ?
                    )");
                    app.bind(1, show->tvdb_id); app.bind(2, show->overview);
                    app.bind(3, show->status);  app.bind(4, show->thumb);
                    app.bind(5, show->art);
                    app.bind(6, show->content_rating);
                    app.bind(7, kairos_id);
                    app.exec();
                }
            } else if (item_type == "movie") {
                auto movie = anidb_->fetchMovie(external_id, language);
                if (movie) {
                    SQLite::Statement app(db_.get(), R"(
                        UPDATE movie SET
                            overview      = CASE WHEN locked THEN overview      ELSE ? END,
                            thumb         = CASE WHEN locked THEN thumb         ELSE ? END,
                            art           = CASE WHEN locked THEN art           ELSE ? END,
                            content_rating= CASE WHEN locked THEN content_rating ELSE ? END
                        WHERE movie_id = ?
                    )");
                    app.bind(1, movie->overview); app.bind(2, movie->thumb);
                    app.bind(3, movie->art);
                    app.bind(4, movie->content_rating);
                    app.bind(5, kairos_id);
                    app.exec();
                }
            }
        }
    if (source == "tmdb" && tmdb_) {
        if (item_type == "show") {
            auto show = tmdb_->fetchShow(external_id, language);
            if (show) {
                linkExternalId(item_type, kairos_id, "tvdb", show->tvdb_id, /*promote_to_primary=*/false);
                linkExternalId(item_type, kairos_id, "imdb", show->imdb_id, /*promote_to_primary=*/false);
                SQLite::Statement app(db_.get(), R"(
                    UPDATE show SET
                        tmdb_id   = CASE WHEN locked THEN tmdb_id   ELSE ? END,
                        tvdb_id   = CASE WHEN locked THEN tvdb_id   ELSE ? END,
                        imdb_id   = CASE WHEN locked THEN imdb_id   ELSE ? END,
                        overview  = CASE WHEN locked THEN overview  ELSE ? END,
                        status    = CASE WHEN locked THEN status    ELSE ? END,
                        genres    = CASE WHEN locked THEN genres    ELSE ? END,
                        network   = CASE WHEN locked THEN network   ELSE ? END,
                        studio    = CASE WHEN locked THEN studio    ELSE ? END,
                        thumb     = CASE WHEN locked THEN thumb     ELSE ? END,
                        art       = CASE WHEN locked THEN art       ELSE ? END
                    WHERE show_id = ?
                )");
                app.bind(1, show->tmdb_id); app.bind(2, show->tvdb_id);
                app.bind(3, show->imdb_id); app.bind(4, show->overview);
                app.bind(5, show->status);  app.bind(6, show->genres);
                app.bind(7, show->network); app.bind(8, show->studio);
                app.bind(9, show->thumb);   app.bind(10, show->art);
                app.bind(11, kairos_id);
                app.exec();
            }
        } else if (item_type == "movie") {
            auto movie = tmdb_->fetchMovie(external_id, language);
            if (movie) {
                linkExternalId(item_type, kairos_id, "imdb", movie->imdb_id, /*promote_to_primary=*/false);
                SQLite::Statement app(db_.get(), R"(
                    UPDATE movie SET
                        tmdb_id      = CASE WHEN locked THEN tmdb_id      ELSE ? END,
                        imdb_id      = CASE WHEN locked THEN imdb_id      ELSE ? END,
                        overview     = CASE WHEN locked THEN overview     ELSE ? END,
                        genres       = CASE WHEN locked THEN genres       ELSE ? END,
                        studio       = CASE WHEN locked THEN studio       ELSE ? END,
                        director     = CASE WHEN locked THEN director     ELSE ? END,
                        release_date = CASE WHEN locked THEN release_date ELSE ? END,
                        thumb        = CASE WHEN locked THEN thumb        ELSE ? END,
                        art          = CASE WHEN locked THEN art          ELSE ? END
                    WHERE movie_id = ?
                )");
                app.bind(1, movie->tmdb_id); app.bind(2, movie->imdb_id);
                app.bind(3, movie->overview); app.bind(4, movie->genres);
                app.bind(5, movie->studio);  app.bind(6, movie->director);
                app.bind(7, movie->release_date);
                app.bind(8, movie->thumb);   app.bind(9, movie->art);
                app.bind(10, kairos_id);
                app.exec();
            }
        }
    }
        if (source == "tvdb" && tvdb_) {
            if (item_type == "show") {
                auto show = tvdb_->fetchShow(external_id, language);
                if (show) {
                    linkExternalId(item_type, kairos_id, "tmdb", show->tmdb_id, /*promote_to_primary=*/false);
                    linkExternalId(item_type, kairos_id, "imdb", show->imdb_id, /*promote_to_primary=*/false);
                    SQLite::Statement app(db_.get(), R"(
                        UPDATE show SET
                            tvdb_id                 = CASE WHEN locked THEN tvdb_id                 ELSE ? END,
                            tmdb_id                 = CASE WHEN locked THEN tmdb_id                 ELSE COALESCE(NULLIF(?,  ''), tmdb_id)  END,
                            imdb_id                 = CASE WHEN locked THEN imdb_id                 ELSE COALESCE(NULLIF(?,  ''), imdb_id)  END,
                            overview                = CASE WHEN locked THEN overview                ELSE ? END,
                            status                  = CASE WHEN locked THEN status                  ELSE ? END,
                            genres                  = CASE WHEN locked THEN genres                  ELSE ? END,
                            network                 = CASE WHEN locked THEN network                 ELSE ? END,
                            originally_available_at = CASE WHEN locked THEN originally_available_at ELSE ? END,
                            year                    = CASE WHEN locked THEN year                    ELSE ? END,
                            thumb                   = CASE WHEN locked THEN thumb                   ELSE ? END,
                            art                     = CASE WHEN locked THEN art                     ELSE ? END
                        WHERE show_id = ?
                    )");
                    app.bind(1,  show->tvdb_id);
                    app.bind(2,  show->tmdb_id);
                    app.bind(3,  show->imdb_id);
                    app.bind(4,  show->overview);
                    app.bind(5,  show->status);
                    app.bind(6,  show->genres);
                    app.bind(7,  show->network);
                    app.bind(8,  show->originally_available_at);
                    if (show->year.has_value()) app.bind(9, show->year.value());
                    else                        app.bind(9);
                    app.bind(10, show->thumb);
                    app.bind(11, show->art);
                    app.bind(12, kairos_id);
                    app.exec();
                }
            } else if (item_type == "movie") {
                auto movie = tvdb_->fetchMovie(external_id, language);
                if (movie) {
                    SQLite::Statement app(db_.get(), R"(
                        UPDATE movie SET
                            tmdb_id      = CASE WHEN locked THEN tmdb_id      ELSE COALESCE(NULLIF(?,  ''), tmdb_id)  END,
                            imdb_id      = CASE WHEN locked THEN imdb_id      ELSE COALESCE(NULLIF(?,  ''), imdb_id)  END,
                            overview     = CASE WHEN locked THEN overview     ELSE ? END,
                            genres       = CASE WHEN locked THEN genres       ELSE ? END,
                            studio       = CASE WHEN locked THEN studio       ELSE ? END,
                            year         = CASE WHEN locked THEN year         ELSE ? END,
                            thumb        = CASE WHEN locked THEN thumb        ELSE ? END,
                            art          = CASE WHEN locked THEN art          ELSE ? END,
                            release_date = CASE WHEN locked THEN release_date ELSE ? END
                        WHERE movie_id = ?
                    )");
                    app.bind(1, movie->tmdb_id);
                    app.bind(2, movie->imdb_id);
                    app.bind(3, movie->overview);
                    app.bind(4, movie->genres);
                    app.bind(5, movie->studio);
                    if (movie->year.has_value()) app.bind(6, movie->year.value());
                    else                         app.bind(6);
                    app.bind(7, movie->thumb);
                    app.bind(8, movie->art);
                    app.bind(9, movie->release_date);
                    app.bind(10, kairos_id);
                    app.exec();
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[scraper] metadata fetch/apply failed for " << kairos_id << " (" << source << "): " << e.what() << "\n";
    }
    return true;
}

bool ScraperManager::rejectCandidate(const std::string& candidate_id) {
    SQLite::Statement rd(db_.get(),
        "SELECT item_type, kairos_id FROM item_match_candidate WHERE candidate_id=?");
    rd.bind(1, candidate_id);
    if (!rd.executeStep()) return false;
    std::string item_type = rd.getColumn(0).getString();
    std::string kairos_id = rd.getColumn(1).getString();

    SQLite::Statement upd(db_.get(),
        "UPDATE item_match_candidate SET accepted=0 WHERE candidate_id=?");
    upd.bind(1, candidate_id); upd.exec();

    // If no pending candidates remain, mark unmatched
    SQLite::Statement chk(db_.get(),
        "SELECT COUNT(*) FROM item_match_candidate "
        "WHERE item_type=? AND kairos_id=? AND accepted IS NULL");
    chk.bind(1, item_type); chk.bind(2, kairos_id);
    if (chk.executeStep() && chk.getColumn(0).getInt() == 0)
        setMatchStatus(item_type, kairos_id, "unmatched", 0.0);

    return true;
}

// ── Review queue ──────────────────────────────────────────────────────────────

std::vector<QueueItem> ScraperManager::getQueue(const std::string& status_filter,
                                                  int limit, int offset) const {
    std::string where;
    if (status_filter == "uncertain") where = " AND match_status='uncertain'";
    else if (status_filter == "unmatched") where = " AND match_status='unmatched'";
    // Auto-matched by the scraper, never reviewed by a human — distinct from a
    // genuinely user-confirmed match (see match_confirmed / acceptCandidate()).
    else if (status_filter == "auto_unconfirmed") where = " AND match_status='matched' AND match_confirmed=0";
    else where = " AND match_status IN ('uncertain','unmatched')";

    // Shows
    std::string sql = R"(
        SELECT s.show_id, 'show', s.title, COALESCE(s.year,0), s.thumb,
               sm.source_id, COALESCE(ms.base_url,''), s.match_status, COALESCE(s.match_score,0)
        FROM show s
        LEFT JOIN source_mapping sm ON sm.kairos_id=s.show_id AND sm.item_type='show'
        LEFT JOIN media_source ms ON ms.source_id=sm.source_id
        WHERE 1=1
    )" + where + R"(
        GROUP BY s.show_id
        ORDER BY s.match_score ASC, s.title ASC
        LIMIT ? OFFSET ?
    )";

    ContentRepository repo(db_);

    std::vector<QueueItem> out;
    {
        SQLite::Statement q(db_.get(), sql);
        q.bind(1, limit); q.bind(2, offset);
        while (q.executeStep()) {
            QueueItem qi;
            qi.kairos_id    = q.getColumn(0).getString();
            qi.item_type    = q.getColumn(1).getString();
            qi.title        = q.getColumn(2).getString();
            qi.year         = q.getColumn(3).getInt();
            qi.thumb        = q.getColumn(4).getString();
            qi.source_id    = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
            qi.source_base_url = q.getColumn(6).isNull() ? "" : q.getColumn(6).getString();
            qi.match_status = q.getColumn(7).getString();
            qi.match_score  = q.getColumn(8).getDouble();
            qi.folder_path  = repo.getShowFolderPath(qi.kairos_id).value_or("");
            out.push_back(std::move(qi));
        }
    }

    // Movies
    sql = R"(
        SELECT m.movie_id, 'movie', m.title, COALESCE(m.year,0), m.thumb,
               sm.source_id, COALESCE(ms.base_url,''), m.match_status, COALESCE(m.match_score,0),
               m.file_path
        FROM movie m
        LEFT JOIN source_mapping sm ON sm.kairos_id=m.movie_id AND sm.item_type='movie'
        LEFT JOIN media_source ms ON ms.source_id=sm.source_id
        WHERE 1=1
    )" + where + R"(
        GROUP BY m.movie_id
        ORDER BY m.match_score ASC, m.title ASC
        LIMIT ? OFFSET ?
    )";
    {
        SQLite::Statement q(db_.get(), sql);
        q.bind(1, limit); q.bind(2, offset);
        while (q.executeStep()) {
            QueueItem qi;
            qi.kairos_id    = q.getColumn(0).getString();
            qi.item_type    = q.getColumn(1).getString();
            qi.title        = q.getColumn(2).getString();
            qi.year         = q.getColumn(3).getInt();
            qi.thumb        = q.getColumn(4).getString();
            qi.source_id    = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
            qi.source_base_url = q.getColumn(6).isNull() ? "" : q.getColumn(6).getString();
            qi.match_status = q.getColumn(7).getString();
            qi.match_score  = q.getColumn(8).getDouble();
            qi.folder_path  = ContentRepository::parentDir(q.getColumn(9).getString());
            out.push_back(std::move(qi));
        }
    }

    // Attach candidates
    for (auto& qi : out) {
        SQLite::Statement cq(db_.get(), R"(
            SELECT candidate_id, source, external_id, title, COALESCE(year,0),
                   score, COALESCE(accepted,-1), poster_url, overview
            FROM item_match_candidate
            WHERE item_type=? AND kairos_id=?
            ORDER BY score DESC
        )");
        cq.bind(1, qi.item_type); cq.bind(2, qi.kairos_id);
        while (cq.executeStep()) {
            ScraperCandidate c;
            c.candidate_id = cq.getColumn(0).getString();
            c.source       = cq.getColumn(1).getString();
            c.external_id  = cq.getColumn(2).getString();
            c.title        = cq.getColumn(3).getString();
            c.year         = cq.getColumn(4).getInt();
            c.score        = cq.getColumn(5).getDouble();
            c.accepted     = cq.getColumn(6).getInt();
            c.poster_url   = cq.getColumn(7).getString();
            c.overview     = cq.getColumn(8).getString();
            qi.candidates.push_back(std::move(c));
        }
    }
    return out;
}

int ScraperManager::queueTotal(const std::string& status_filter) const {
    std::string where;
    if (status_filter == "uncertain") where = " match_status='uncertain'";
    else if (status_filter == "unmatched") where = " match_status='unmatched'";
    else if (status_filter == "auto_unconfirmed") where = " match_status='matched' AND match_confirmed=0";
    else where = " match_status IN ('uncertain','unmatched')";

    SQLite::Statement q(db_.get(),
        "SELECT (SELECT COUNT(*) FROM show WHERE " + where + ") "
        "+ (SELECT COUNT(*) FROM movie WHERE " + where + ")");
    if (q.executeStep()) return q.getColumn(0).getInt();
    return 0;
}

// ── Manual match ──────────────────────────────────────────────────────────────

bool ScraperManager::manualMatch(const std::string& kairos_id,
                                  const std::string& item_type,
                                  const std::string& source,
                                  const std::string& external_id,
                                  const std::string& title,
                                  int year,
                                  const std::string& poster_url,
                                  const std::string& overview) {
    storeCandidate(item_type, kairos_id, source, external_id, title, year, 1.0, poster_url, overview);
    std::string cid = candidateKey(item_type, kairos_id, source, external_id);
    return acceptCandidate(cid);
}

bool ScraperManager::refreshMetadata(const std::string& kairos_id, const std::string& item_type) {
    // Resolve effective metadata language: item-level override → library default → "".
    std::string language;
    try {
        const std::string lang_sql = item_type == "show" ? R"(
            SELECT COALESCE(NULLIF(s.preferred_language,''),
                            NULLIF(ml.preferred_language,''), '')
            FROM show s
            LEFT JOIN source_mapping sm ON sm.kairos_id = s.show_id AND sm.item_type = 'show'
            LEFT JOIN media_library ml ON ml.library_id = sm.library_id
            WHERE s.show_id = ? LIMIT 1
        )" : R"(
            SELECT COALESCE(NULLIF(m.preferred_language,''),
                            NULLIF(ml.preferred_language,''), '')
            FROM movie m
            LEFT JOIN source_mapping sm ON sm.kairos_id = m.movie_id AND sm.item_type = 'movie'
            LEFT JOIN media_library ml ON ml.library_id = sm.library_id
            WHERE m.movie_id = ? LIMIT 1
        )";
        SQLite::Statement lq(db_.get(), lang_sql);
        lq.bind(1, kairos_id);
        if (lq.executeStep()) language = lq.getColumn(0).getString();
    } catch (...) {}

    auto ids = getExternalIds(kairos_id, item_type);
    if (ids.empty()) return false;

    bool any_success = false;
    // getExternalIds() returns priority ascending (1 = primary, per the
    // "Priority & Identifiers" editor). Every UPDATE below only overwrites a
    // field when the freshly-fetched value is non-empty, so applying in that
    // same ascending order meant whichever source was processed LAST (i.e.
    // the lowest-priority one) always won for any field two sources both
    // populate — the opposite of what the priority list promises. Iterating
    // in reverse makes the highest-priority source the last write, so it
    // wins for the fields it has, and lower-priority sources only fill in
    // whatever it leaves blank.
    for (auto rit = ids.rbegin(); rit != ids.rend(); ++rit) {
        const auto& id = *rit;
        try {
            if (id.source == "tmdb" && tmdb_) {
                if (item_type == "show") {
                    auto show = tmdb_->fetchShow(id.external_id, language);
                    if (show) {
                        linkExternalId(item_type, kairos_id, "tvdb", show->tvdb_id, /*promote_to_primary=*/false);
                        linkExternalId(item_type, kairos_id, "imdb", show->imdb_id, /*promote_to_primary=*/false);
                        SQLite::Statement app(db_.get(), R"(
                            UPDATE show SET
                                tmdb_id   = CASE WHEN locked THEN tmdb_id   ELSE COALESCE(NULLIF(?, ''), tmdb_id) END,
                                tvdb_id   = CASE WHEN locked THEN tvdb_id   ELSE COALESCE(NULLIF(?, ''), tvdb_id) END,
                                imdb_id   = CASE WHEN locked THEN imdb_id   ELSE COALESCE(NULLIF(?, ''), imdb_id) END,
                                overview  = CASE WHEN locked THEN overview  ELSE COALESCE(NULLIF(?, ''), overview) END,
                                status    = CASE WHEN locked THEN status    ELSE COALESCE(NULLIF(?, ''), status)   END,
                                genres    = CASE WHEN locked THEN genres    ELSE COALESCE(NULLIF(?, '[]'), genres) END,
                                network   = CASE WHEN locked THEN network   ELSE COALESCE(NULLIF(?, ''), network)  END,
                                studio    = CASE WHEN locked THEN studio    ELSE COALESCE(NULLIF(?, ''), studio)   END,
                                thumb     = CASE WHEN locked THEN thumb     ELSE COALESCE(NULLIF(?, ''), thumb)    END,
                                art       = CASE WHEN locked THEN art       ELSE COALESCE(NULLIF(?, ''), art)      END
                            WHERE show_id = ?
                        )");
                        app.bind(1, show->tmdb_id); app.bind(2, show->tvdb_id);
                        app.bind(3, show->imdb_id); app.bind(4, show->overview);
                        app.bind(5, show->status);  app.bind(6, show->genres);
                        app.bind(7, show->network); app.bind(8, show->studio);
                        app.bind(9, show->thumb);   app.bind(10, show->art);
                        app.bind(11, kairos_id);
                        app.exec();
                        any_success = true;
                    }
                } else if (item_type == "movie") {
                    auto movie = tmdb_->fetchMovie(id.external_id, language);
                    if (movie) {
                        linkExternalId(item_type, kairos_id, "imdb", movie->imdb_id, /*promote_to_primary=*/false);
                        SQLite::Statement app(db_.get(), R"(
                            UPDATE movie SET
                                tmdb_id      = CASE WHEN locked THEN tmdb_id      ELSE COALESCE(NULLIF(?, ''), tmdb_id)      END,
                                imdb_id      = CASE WHEN locked THEN imdb_id      ELSE COALESCE(NULLIF(?, ''), imdb_id)      END,
                                overview     = CASE WHEN locked THEN overview     ELSE COALESCE(NULLIF(?, ''), overview)     END,
                                genres       = CASE WHEN locked THEN genres       ELSE COALESCE(NULLIF(?, '[]'), genres)     END,
                                studio       = CASE WHEN locked THEN studio       ELSE COALESCE(NULLIF(?, ''), studio)       END,
                                director     = CASE WHEN locked THEN director     ELSE COALESCE(NULLIF(?, ''), director)     END,
                                release_date = CASE WHEN locked THEN release_date ELSE COALESCE(NULLIF(?, ''), release_date) END,
                                thumb        = CASE WHEN locked THEN thumb        ELSE COALESCE(NULLIF(?, ''), thumb)        END,
                                art          = CASE WHEN locked THEN art          ELSE COALESCE(NULLIF(?, ''), art)          END
                            WHERE movie_id = ?
                        )");
                        app.bind(1, movie->tmdb_id); app.bind(2, movie->imdb_id);
                        app.bind(3, movie->overview); app.bind(4, movie->genres);
                        app.bind(5, movie->studio);  app.bind(6, movie->director);
                        app.bind(7, movie->release_date);
                        app.bind(8, movie->thumb);   app.bind(9, movie->art);
                        app.bind(10, kairos_id);
                        app.exec();
                        any_success = true;
                    }
                }
            } else if (id.source == "tvdb" && tvdb_) {
                if (item_type == "show") {
                    auto show = tvdb_->fetchShow(id.external_id, language);
                    if (show) {
                        linkExternalId(item_type, kairos_id, "tmdb", show->tmdb_id, /*promote_to_primary=*/false);
                        linkExternalId(item_type, kairos_id, "imdb", show->imdb_id, /*promote_to_primary=*/false);
                        SQLite::Statement app(db_.get(), R"(
                            UPDATE show SET
                                tvdb_id  = CASE WHEN locked THEN tvdb_id  ELSE COALESCE(NULLIF(?, ''), tvdb_id) END,
                                tmdb_id  = CASE WHEN locked THEN tmdb_id  ELSE COALESCE(NULLIF(?, ''), tmdb_id) END,
                                imdb_id  = CASE WHEN locked THEN imdb_id  ELSE COALESCE(NULLIF(?, ''), imdb_id) END,
                                overview = CASE WHEN locked THEN overview  ELSE COALESCE(NULLIF(?, ''), overview) END,
                                status   = CASE WHEN locked THEN status   ELSE COALESCE(NULLIF(?, ''), status)   END,
                                network  = CASE WHEN locked THEN network  ELSE COALESCE(NULLIF(?, ''), network)  END,
                                thumb    = CASE WHEN locked THEN thumb    ELSE COALESCE(NULLIF(?, ''), thumb)    END,
                                genres   = CASE WHEN locked THEN genres   ELSE COALESCE(NULLIF(?, '[]'), genres) END
                            WHERE show_id = ?
                        )");
                        app.bind(1, show->tvdb_id); app.bind(2, show->tmdb_id);
                        app.bind(3, show->imdb_id); app.bind(4, show->overview);
                        app.bind(5, show->status);  app.bind(6, show->network);
                        app.bind(7, show->thumb);   app.bind(8, show->genres);
                        app.bind(9, kairos_id);
                        app.exec();
                        any_success = true;
                    }
                } else if (item_type == "movie") {
                    auto movie = tvdb_->fetchMovie(id.external_id, language);
                    if (movie) {
                        linkExternalId(item_type, kairos_id, "tmdb", movie->tmdb_id, /*promote_to_primary=*/false);
                        linkExternalId(item_type, kairos_id, "imdb", movie->imdb_id, /*promote_to_primary=*/false);
                        SQLite::Statement app(db_.get(), R"(
                            UPDATE movie SET
                                tmdb_id      = CASE WHEN locked THEN tmdb_id      ELSE COALESCE(NULLIF(?, ''), tmdb_id)      END,
                                imdb_id      = CASE WHEN locked THEN imdb_id      ELSE COALESCE(NULLIF(?, ''), imdb_id)      END,
                                overview     = CASE WHEN locked THEN overview     ELSE COALESCE(NULLIF(?, ''), overview)     END,
                                release_date = CASE WHEN locked THEN release_date ELSE COALESCE(NULLIF(?, ''), release_date) END,
                                thumb        = CASE WHEN locked THEN thumb        ELSE COALESCE(NULLIF(?, ''), thumb)        END,
                                genres       = CASE WHEN locked THEN genres       ELSE COALESCE(NULLIF(?, '[]'), genres)     END,
                                studio       = CASE WHEN locked THEN studio       ELSE COALESCE(NULLIF(?, ''), studio)       END
                            WHERE movie_id = ?
                        )");
                        app.bind(1, movie->tmdb_id); app.bind(2, movie->imdb_id);
                        app.bind(3, movie->overview); app.bind(4, movie->release_date);
                        app.bind(5, movie->thumb);   app.bind(6, movie->genres);
                        app.bind(7, movie->studio);  app.bind(8, kairos_id);
                        app.exec();
                        any_success = true;
                    }
                }
            } else if (id.source == "anidb" && anidb_) {
                if (item_type == "show") {
                    auto show = anidb_->fetchShow(id.external_id, language);
                    if (show) {
                        linkExternalId(item_type, kairos_id, "tvdb", show->tvdb_id, /*promote_to_primary=*/false);
                        SQLite::Statement app(db_.get(), R"(
                            UPDATE show SET
                                tvdb_id        = CASE WHEN locked THEN tvdb_id        ELSE COALESCE(NULLIF(?, ''), tvdb_id)        END,
                                overview       = CASE WHEN locked THEN overview       ELSE COALESCE(NULLIF(?, ''), overview)       END,
                                status         = CASE WHEN locked THEN status         ELSE COALESCE(NULLIF(?, ''), status)         END,
                                thumb          = CASE WHEN locked THEN thumb          ELSE COALESCE(NULLIF(?, ''), thumb)          END,
                                art            = CASE WHEN locked THEN art            ELSE COALESCE(NULLIF(?, ''), art)            END,
                                content_rating = CASE WHEN locked THEN content_rating ELSE COALESCE(NULLIF(?, ''), content_rating) END
                            WHERE show_id = ?
                        )");
                        app.bind(1, show->tvdb_id); app.bind(2, show->overview);
                        app.bind(3, show->status);  app.bind(4, show->thumb);
                        app.bind(5, show->art);     app.bind(6, show->content_rating);
                        app.bind(7, kairos_id);
                        app.exec();
                        any_success = true;
                    }
                } else if (item_type == "movie") {
                    auto movie = anidb_->fetchMovie(id.external_id, language);
                    if (movie) {
                        SQLite::Statement app(db_.get(), R"(
                            UPDATE movie SET
                                overview       = CASE WHEN locked THEN overview       ELSE COALESCE(NULLIF(?, ''), overview)       END,
                                thumb          = CASE WHEN locked THEN thumb          ELSE COALESCE(NULLIF(?, ''), thumb)          END,
                                art            = CASE WHEN locked THEN art            ELSE COALESCE(NULLIF(?, ''), art)            END,
                                content_rating = CASE WHEN locked THEN content_rating ELSE COALESCE(NULLIF(?, ''), content_rating) END
                            WHERE movie_id = ?
                        )");
                        app.bind(1, movie->overview); app.bind(2, movie->thumb);
                        app.bind(3, movie->art);     app.bind(4, movie->content_rating);
                        app.bind(5, kairos_id);
                        app.exec();
                        any_success = true;
                    }
                }
            }
        } catch (...) {}
    }
    return any_success;
}

// ── Live search ───────────────────────────────────────────────────────────────

std::vector<ScraperManager::SearchResult>
ScraperManager::search(const std::string& query, const std::string& content_type) const {
    std::vector<SearchResult> out;

    auto addShow = [&](const std::string& source, const Show& s) {
        SearchResult r;
        r.source      = source;
        if (source == "tmdb")       r.external_id = s.tmdb_id;
        else if (source == "tvdb")  r.external_id = s.tvdb_id;
        else if (source == "anidb") r.external_id = s.show_id;
        r.title        = s.title;
        r.year         = s.year.has_value() ? s.year.value() : 0;
        r.overview     = s.overview;
        r.poster_url   = s.thumb;
        r.content_type = "show";
        // Check if in library by tmdb_id, tvdb_id, or item_external_id
        std::string found_id;
        auto checkId = [&](const std::string& src, const std::string& val) {
            if (val.empty() || !found_id.empty()) return;
            SQLite::Statement chk(db_.get(),
                "SELECT kairos_id FROM item_external_id WHERE item_type='show' AND source=? AND external_id=? LIMIT 1");
            chk.bind(1, src); chk.bind(2, val);
            if (chk.executeStep()) found_id = chk.getColumn(0).getString();
        };
        checkId(source, r.external_id);
        checkId("tmdb", s.tmdb_id);
        checkId("tvdb", s.tvdb_id);
        checkId("imdb", s.imdb_id);

        if (!found_id.empty()) {
            r.in_library = true;
            r.library_id = found_id;
        }

        // Already requested by anyone (not just the current caller) — lets a
        // requester see "already in the system" even when it's not in_library
        // yet (requested but not fulfilled). Prefer the most "advanced" status.
        if (!r.external_id.empty()) {
            SQLite::Statement rq(db_.get(),
                "SELECT status FROM content_request WHERE content_type='show' AND source=? AND external_id=? "
                "ORDER BY CASE status WHEN 'approved' THEN 0 WHEN 'pending' THEN 1 ELSE 2 END LIMIT 1");
            rq.bind(1, source); rq.bind(2, r.external_id);
            if (rq.executeStep()) r.request_status = rq.getColumn(0).getString();
        }
        if (!r.external_id.empty()) out.push_back(std::move(r));
    };

    auto addMovie = [&](const std::string& source, const Movie& m) {
        SearchResult r;
        r.source      = source;
        if (source == "tmdb")       r.external_id = m.tmdb_id;
        else if (source == "tvdb")  r.external_id = m.imdb_id;
        else if (source == "anidb") r.external_id = m.movie_id;
        r.title        = m.title;
        r.year         = m.year.has_value() ? m.year.value() : 0;
        r.overview     = m.overview;
        r.poster_url   = m.thumb;
        r.content_type = "movie";

        std::string found_id;
        auto checkId = [&](const std::string& src, const std::string& val) {
            if (val.empty() || !found_id.empty()) return;
            SQLite::Statement chk(db_.get(),
                "SELECT kairos_id FROM item_external_id WHERE item_type='movie' AND source=? AND external_id=? LIMIT 1");
            chk.bind(1, src); chk.bind(2, val);
            if (chk.executeStep()) found_id = chk.getColumn(0).getString();
        };
        checkId(source, r.external_id);
        checkId("tmdb", m.tmdb_id);
        checkId("imdb", m.imdb_id);

        if (!found_id.empty()) {
            r.in_library = true;
            r.library_id = found_id;
        }

        if (!r.external_id.empty()) {
            SQLite::Statement rq(db_.get(),
                "SELECT status FROM content_request WHERE content_type='movie' AND source=? AND external_id=? "
                "ORDER BY CASE status WHEN 'approved' THEN 0 WHEN 'pending' THEN 1 ELSE 2 END LIMIT 1");
            rq.bind(1, source); rq.bind(2, r.external_id);
            if (rq.executeStep()) r.request_status = rq.getColumn(0).getString();
        }
        if (!r.external_id.empty()) out.push_back(std::move(r));
    };

    // Direct ID lookup: "tmdb:NNN", "tvdb:NNN", or "anidb:NNN"
    std::string id_source, id_value;
    if (query.starts_with("tmdb:") && query.size() > 5) {
        id_source = "tmdb"; id_value = query.substr(5);
    } else if (query.starts_with("tvdb:") && query.size() > 5) {
        id_source = "tvdb"; id_value = query.substr(5);
    } else if (query.starts_with("anidb:") && query.size() > 6) {
        id_source = "anidb"; id_value = query.substr(6);
    }
    if (!id_value.empty()) {
        if (id_source == "tmdb" && tmdb_) {
            if (content_type == "show" || content_type.empty()) {
                auto s = tmdb_->fetchShow(id_value);
                if (s) addShow("tmdb", *s);
            }
            if (content_type == "movie" || content_type.empty()) {
                auto m = tmdb_->fetchMovie(id_value);
                if (m) addMovie("tmdb", *m);
            }
        }
        if (id_source == "tvdb" && tvdb_) {
            if (content_type == "show" || content_type.empty()) {
                auto s = tvdb_->fetchShow(id_value);
                if (s) addShow("tvdb", *s);
            }
            if (content_type == "movie" || content_type.empty()) {
                auto m = tvdb_->fetchMovie(id_value);
                if (m) addMovie("tvdb", *m);
            }
        }
        if (id_source == "anidb" && anidb_) {
            if (content_type == "show" || content_type.empty()) {
                auto s = anidb_->fetchShow(id_value);
                if (s) addShow("anidb", *s);
            }
            if (content_type == "movie" || content_type.empty()) {
                auto m = anidb_->fetchMovie(id_value);
                if (m) addMovie("anidb", *m);
            }
        }
        return out;
    }

    // Text search
    if (content_type == "show" || content_type.empty()) {
        if (tmdb_)  for (auto& s : tmdb_->searchShows(query))  addShow("tmdb",  s);
        if (tvdb_)  for (auto& s : tvdb_->searchShows(query))  addShow("tvdb",  s);
        if (anidb_) for (auto& s : anidb_->searchShows(query)) addShow("anidb", s);
    }
    if (content_type == "movie" || content_type.empty()) {
        if (tmdb_)  for (auto& m : tmdb_->searchMovies(query))  addMovie("tmdb",  m);
        if (tvdb_)  for (auto& m : tvdb_->searchMovies(query))  addMovie("tvdb",  m);
        if (anidb_) for (auto& m : anidb_->searchMovies(query)) addMovie("anidb", m);
    }
    return out;
}

// ── AniDB poster URL ──────────────────────────────────────────────────────────

std::string ScraperManager::anidbPosterUrl(const std::string& aid) const {
    if (!anidb_) return {};
    return anidb_->posterUrl(aid);
}

void ScraperManager::anidbRateLimitImage() const {
    if (anidb_) anidb_->rateLimitImageWait();
}

// ── Stats ─────────────────────────────────────────────────────────────────────

ScraperStats ScraperManager::stats() const {
    ScraperStats s;
    auto count = [&](const std::string& tbl, const std::string& status) {
        SQLite::Statement q(db_.get(),
            "SELECT COUNT(*) FROM " + tbl + " WHERE match_status=?");
        q.bind(1, status);
        return q.executeStep() ? q.getColumn(0).getInt() : 0;
    };
    // Subset of "unscraped" that will never actually be attempted, because the
    // item or its library opts out of scraping — reported separately so
    // "unscraped" keeps meaning "pending," not "opted out forever."
    auto countSkipped = [&](const std::string& tbl) {
        std::string sql =
            "SELECT COUNT(*) FROM " + tbl + " t "
            "LEFT JOIN source_mapping sm ON sm.kairos_id = t." + tbl + "_id AND sm.item_type = '" + tbl + "' "
            "LEFT JOIN media_library ml ON ml.library_id = sm.library_id "
            "WHERE t.match_status = 'unscraped' AND (t.skip_scraping = 1 OR COALESCE(ml.skip_scraping, 0) = 1)";
        SQLite::Statement q(db_.get(), sql);
        return q.executeStep() ? q.getColumn(0).getInt() : 0;
    };
    for (const auto* tbl : { "show", "movie" }) {
        int matched_c   = count(tbl, "matched");
        int uncertain_c = count(tbl, "uncertain");
        int unmatched_c = count(tbl, "unmatched");
        int unscraped_c = count(tbl, "unscraped");
        int skipped_c   = countSkipped(tbl);

        s.total     += matched_c + uncertain_c + unmatched_c + unscraped_c;
        s.matched   += matched_c;
        s.uncertain += uncertain_c;
        s.unmatched += unmatched_c;
        s.unscraped += unscraped_c - skipped_c;
        s.skipped   += skipped_c;
    }
    return s;
}
