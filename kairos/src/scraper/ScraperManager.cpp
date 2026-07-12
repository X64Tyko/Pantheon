#include "ScraperManager.h"
#include "AnidbScraper.h"
#include "TmdbScraper.h"
#include "TvdbScraper.h"
#include "conf/ConfStore.h"
#include "db/ContentRepository.h"
#include "db/Database.h"
#include "db/SourceRepository.h"
#include "log/DebugLog.h"
#include "util/TitleMatch.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

namespace fs = std::filesystem;

using json = nlohmann::json;

// ── Title normalisation + similarity ─────────────────────────────────────────
// normalizeTitle/titleSimilarity live in util/TitleMatch.h now — shared with
// SyncManager's cross-source sync-time dedup.

namespace {

using titlematch::normalizeTitle;
using titlematch::titleSimilarity;

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// A local folder/file name is very often the canonical title plus a handful
// of extra descriptive words a scraper's own title doesn't carry — "Batman"
// vs the on-disk "Batman TV Series 1966", "Show Name" vs a "Show Name
// Complete Season 1" that slipped past parseTitle's junk-tag cut. Plain
// character-edit-distance-over-length (titleSimilarity) punishes this out of
// proportion to how confident a match it actually is, especially for short
// titles where a couple of extra words dominate the length. When one title's
// words are a whole-word prefix of the other's, score it near the top instead
// — tapering by extra-word count, and giving up (falling back to
// titleSimilarity) past 5 extra words, where an unrelated tail is the more
// likely explanation than an over-descriptive folder name.
//
// Deliberately NOT folded into titlematch::titleSimilarity() itself: that
// function is shared with SyncManager's cross-source dedup, which has no
// human review step to catch a false "these are the same show" merge, so it
// can't afford this leniency the way an uncertain-queue candidate can.
double titlePrefixSimilarity(const std::string& a, const std::string& b) {
    auto wa = splitWords(normalizeTitle(a));
    auto wb = splitWords(normalizeTitle(b));
    const auto& shorter = wa.size() <= wb.size() ? wa : wb;
    const auto& longer  = wa.size() <= wb.size() ? wb : wa;
    if (shorter.empty() || shorter.size() == longer.size()) return 0.0;
    size_t extra = longer.size() - shorter.size();
    if (extra > 5) return 0.0;
    if (!std::equal(shorter.begin(), shorter.end(), longer.begin())) return 0.0;
    return 1.0 - static_cast<double>(extra) * 0.08;
}

double computeScore(const std::string& source_title, int source_year,
                    const std::string& cand_title,  int cand_year) {
    double ts = std::max(titleSimilarity(source_title, cand_title),
                         titlePrefixSimilarity(source_title, cand_title));
    double yb = 0.0;
    if (source_year > 0 && cand_year > 0) {
        if (source_year == cand_year) yb = 1.0;
        else if (std::abs(source_year - cand_year) <= 1) yb = 0.4;
    } else {
        yb = 0.3; // unknown year on one side — partial credit
    }
    return ts * 0.75 + yb * 0.25;
}

// ── Metadata writeback ───────────────────────────────────────────────────────
// Applies a freshly-fetched Show/Movie onto the matching row. Single source of
// truth for which columns a match/refresh actually writes — acceptCandidate()
// and refreshMetadata() used to each carry their own hand-rolled UPDATE per
// source (tmdb/tvdb/anidb), and the three copies had silently drifted apart:
// none of them ever wrote `title`, only the tvdb-show one wrote `year`, and
// only the anidb ones wrote `content_rating`/`audience_rating` — so a match
// could "succeed" (poster, overview, etc. all update) while the title stayed
// whatever a bad auto-match had set it to. Every field the Show/Movie struct
// can actually carry is written here, once, so every source/caller gets the
// same coverage. `locked` (a manual per-item protection flag, not related to
// which field is empty) still gates all of it, same as before.
void applyShowMetadata(SQLite::Database& db, const std::string& kairos_id, const Show& s) {
    SQLite::Statement app(db, R"(
        UPDATE show SET
            title                   = CASE WHEN locked THEN title                   ELSE COALESCE(NULLIF(?, ''), title)                   END,
            tmdb_id                 = CASE WHEN locked THEN tmdb_id                 ELSE COALESCE(NULLIF(?, ''), tmdb_id)                 END,
            tvdb_id                 = CASE WHEN locked THEN tvdb_id                 ELSE COALESCE(NULLIF(?, ''), tvdb_id)                 END,
            imdb_id                 = CASE WHEN locked THEN imdb_id                 ELSE COALESCE(NULLIF(?, ''), imdb_id)                 END,
            overview                = CASE WHEN locked THEN overview                ELSE COALESCE(NULLIF(?, ''), overview)                END,
            status                  = CASE WHEN locked THEN status                  ELSE COALESCE(NULLIF(?, ''), status)                  END,
            genres                  = CASE WHEN locked THEN genres                  ELSE COALESCE(NULLIF(?, '[]'), genres)                END,
            network                 = CASE WHEN locked THEN network                 ELSE COALESCE(NULLIF(?, ''), network)                 END,
            studio                  = CASE WHEN locked THEN studio                  ELSE COALESCE(NULLIF(?, ''), studio)                  END,
            content_rating          = CASE WHEN locked THEN content_rating          ELSE COALESCE(NULLIF(?, ''), content_rating)          END,
            originally_available_at = CASE WHEN locked THEN originally_available_at ELSE COALESCE(NULLIF(?, ''), originally_available_at) END,
            year                    = CASE WHEN locked THEN year                    ELSE COALESCE(?, year)                                END,
            audience_rating         = CASE WHEN locked THEN audience_rating         ELSE COALESCE(?, audience_rating)                      END,
            thumb                   = CASE WHEN locked THEN thumb                   ELSE COALESCE(NULLIF(?, ''), thumb)                    END,
            art                     = CASE WHEN locked THEN art                     ELSE COALESCE(NULLIF(?, ''), art)                      END
        WHERE show_id = ?
    )");
    int i = 1;
    app.bind(i++, s.title);
    app.bind(i++, s.tmdb_id);
    app.bind(i++, s.tvdb_id);
    app.bind(i++, s.imdb_id);
    app.bind(i++, s.overview);
    app.bind(i++, s.status);
    app.bind(i++, s.genres);
    app.bind(i++, s.network);
    app.bind(i++, s.studio);
    app.bind(i++, s.content_rating);
    app.bind(i++, s.originally_available_at);
    if (s.year.has_value())            app.bind(i++, s.year.value());            else app.bind(i++);
    if (s.audience_rating.has_value()) app.bind(i++, s.audience_rating.value()); else app.bind(i++);
    app.bind(i++, s.thumb);
    app.bind(i++, s.art);
    app.bind(i++, kairos_id);
    app.exec();
}

void applyMovieMetadata(SQLite::Database& db, const std::string& kairos_id, const Movie& m) {
    SQLite::Statement app(db, R"(
        UPDATE movie SET
            title           = CASE WHEN locked THEN title           ELSE COALESCE(NULLIF(?, ''), title)           END,
            tmdb_id         = CASE WHEN locked THEN tmdb_id         ELSE COALESCE(NULLIF(?, ''), tmdb_id)         END,
            imdb_id         = CASE WHEN locked THEN imdb_id         ELSE COALESCE(NULLIF(?, ''), imdb_id)         END,
            overview        = CASE WHEN locked THEN overview        ELSE COALESCE(NULLIF(?, ''), overview)        END,
            tagline         = CASE WHEN locked THEN tagline         ELSE COALESCE(NULLIF(?, ''), tagline)         END,
            genres          = CASE WHEN locked THEN genres          ELSE COALESCE(NULLIF(?, '[]'), genres)        END,
            studio          = CASE WHEN locked THEN studio          ELSE COALESCE(NULLIF(?, ''), studio)          END,
            director        = CASE WHEN locked THEN director        ELSE COALESCE(NULLIF(?, ''), director)        END,
            content_rating  = CASE WHEN locked THEN content_rating  ELSE COALESCE(NULLIF(?, ''), content_rating)  END,
            release_date    = CASE WHEN locked THEN release_date    ELSE COALESCE(NULLIF(?, ''), release_date)    END,
            year            = CASE WHEN locked THEN year            ELSE COALESCE(?, year)                       END,
            audience_rating = CASE WHEN locked THEN audience_rating ELSE COALESCE(?, audience_rating)             END,
            thumb           = CASE WHEN locked THEN thumb           ELSE COALESCE(NULLIF(?, ''), thumb)           END,
            art             = CASE WHEN locked THEN art             ELSE COALESCE(NULLIF(?, ''), art)             END
        WHERE movie_id = ?
    )");
    int i = 1;
    app.bind(i++, m.title);
    app.bind(i++, m.tmdb_id);
    app.bind(i++, m.imdb_id);
    app.bind(i++, m.overview);
    app.bind(i++, m.tagline);
    app.bind(i++, m.genres);
    app.bind(i++, m.studio);
    app.bind(i++, m.director);
    app.bind(i++, m.content_rating);
    app.bind(i++, m.release_date);
    if (m.year.has_value())            app.bind(i++, m.year.value());            else app.bind(i++);
    if (m.audience_rating.has_value()) app.bind(i++, m.audience_rating.value()); else app.bind(i++);
    app.bind(i++, m.thumb);
    app.bind(i++, m.art);
    app.bind(i++, kairos_id);
    app.exec();
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

// Cleans a raw on-disk folder name down to a bare title, for comparison
// against (and, on mismatch, searching with) a source-supplied title.
// Delegates to the same scene-release-aware parser LocalSource uses to
// build titles in the first place (strips "(YYYY)", quality/codec tags,
// and "Complete"/"Season N"/"TV Series"-style collection descriptors) —
// using anything weaker here (e.g. a plain trailing-"(YYYY)" strip) means a
// messy real-world folder name never looks similar enough to the
// already-cleaned title below, and the mismatch check "recovers" by
// searching TMDB/TVDB with the raw junk-laden folder name instead of a
// clean one. See TitleMatch.h for why this lives there.
std::string cleanFolderTitle(const std::string& name) {
    return titlematch::parseReleaseTitle(name).first;
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

// Rough day-count for a "YYYY-MM-DD" date string, for distance comparisons
// only (not calendar-correct, just monotonic enough to diff).
long dateToDayCount(const std::string& d) {
    if (d.size() < 10) return 0;
    try {
        std::tm tm{};
        tm.tm_year = std::stoi(d.substr(0, 4)) - 1900;
        tm.tm_mon  = std::stoi(d.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(d.substr(8, 2));
        return static_cast<long>(std::mktime(&tm)) / 86400;
    } catch (...) { return 0; }
}

// Whether two season-0 specials found via different scrapers are likely the
// same underlying special — high title similarity, and (when both sides
// have a date) air dates within ~60 days. Titles alone aren't enough:
// different specials of the same show ("Recap Special", "OVA 1") are
// often similarly generic-titled.
bool sameSpecial(const std::string& title_a, const std::string& air_date_a,
                  const std::string& title_b, const std::string& air_date_b) {
    if (titleSimilarity(title_a, title_b) < 0.75) return false;
    long da = dateToDayCount(air_date_a), db2 = dateToDayCount(air_date_b);
    if (da == 0 || db2 == 0) return true; // no reliable date on one side — trust title alone
    return std::labs(da - db2) <= 60;
}

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
    s.dedup_fuzzy_title_threshold          = std::stod(readKey("dedup_fuzzy_title_threshold", "0.80"));
    s.dedup_folder_corroboration_threshold = std::stod(readKey("dedup_folder_corroboration_threshold", "0.30"));
    s.anidb_download_posters               = readKey("anidb_download_posters", "0") == "1";

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
    writeKey("dedup_fuzzy_title_threshold",          std::to_string(s.dedup_fuzzy_title_threshold));
    writeKey("dedup_folder_corroboration_threshold", std::to_string(s.dedup_folder_corroboration_threshold));
    writeKey("anidb_download_posters", s.anidb_download_posters ? "1" : "0");
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

std::string ScraperManager::findExternalIdOwner(const std::string& item_type,
                                                 const std::string& source,
                                                 const std::string& external_id,
                                                 const std::string& exclude_kairos_id) const {
    if (external_id.empty()) return "";
    const std::string id_col = item_type + "_id"; // "show_id" | "movie_id"
    SQLite::Statement q(db_.get(),
        "SELECT ie.kairos_id FROM item_external_id ie "
        "JOIN " + item_type + " t ON t." + id_col + " = ie.kairos_id "
        "WHERE ie.item_type = ? AND ie.source = ? AND ie.external_id = ? AND ie.kairos_id != ? "
        "LIMIT 1");
    q.bind(1, item_type); q.bind(2, source); q.bind(3, external_id); q.bind(4, exclude_kairos_id);
    if (q.executeStep()) return q.getColumn(0).getString();
    return "";
}

ScraperManager::DuplicateMergeResult
ScraperManager::mergeIfDuplicateExternalId(const std::string& item_type,
                                            const std::string& kairos_id,
                                            const std::string& source,
                                            const std::string& external_id) {
    DuplicateMergeResult result;
    std::string owner = findExternalIdOwner(item_type, source, external_id, kairos_id);
    if (owner.empty()) return result;

    ContentRepository repo(db_);
    std::string dup_title, dup_folder, owner_title, owner_folder;

    if (item_type == "show") {
        auto d_dup = repo.getShowDetail(kairos_id);
        auto d_own = repo.getShowDetail(owner);
        // Defensive: don't merge against a row that's already gone (e.g. an
        // ordering artifact within the same runMatch pass).
        if (!d_dup || !d_own) return result;
        dup_title = d_dup->title; dup_folder = d_dup->folder_path;
        owner_title = d_own->title; owner_folder = d_own->folder_path;
        result.folder_mismatch = !dup_folder.empty() && !owner_folder.empty() && dup_folder != owner_folder;
        repo.mergeShowInto(owner, kairos_id);
    } else {
        auto d_dup = repo.getMovieDetail(kairos_id);
        auto d_own = repo.getMovieDetail(owner);
        if (!d_dup || !d_own) return result;
        dup_title = d_dup->title; dup_folder = d_dup->folder_path;
        owner_title = d_own->title; owner_folder = d_own->folder_path;
        result.folder_mismatch = !dup_folder.empty() && !owner_folder.empty() && dup_folder != owner_folder;
        repo.mergeMovieInto(owner, kairos_id);
    }

    std::cout << "[scraper] auto-merge: " << item_type << " \"" << dup_title << "\" (" << kairos_id
              << ") duplicates existing " << item_type << " \"" << owner_title << "\" (" << owner
              << ") via " << source << ":" << external_id << "\n";
    if (result.folder_mismatch) {
        std::cerr << "[scraper] WARNING: auto-merge forced past folder_path mismatch: \""
                   << dup_folder << "\" vs \"" << owner_folder << "\" (" << item_type << " "
                   << kairos_id << " -> " << owner << ")\n";
    }
    result.merged_into_kairos_id = owner;
    result.merged_into_title     = owner_title;
    return result;
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

// ── Trigger / background metadata refresh ───────────────────────────────────

void ScraperManager::triggerRefreshAll() {
    bool expected = false;
    if (!refreshing_all_.compare_exchange_strong(expected, true)) {
        std::cout << "[scraper] metadata refresh already running — ignoring trigger\n";
        return;
    }
    std::thread([this]() {
        try { runRefreshAll(); }
        catch (const std::exception& e) {
            std::cerr << "[scraper] refresh-all error: " << e.what() << "\n";
        }
        refreshing_all_.store(false);
    }).detach();
}

void ScraperManager::runRefreshAll() {
    std::vector<std::string> show_ids, movie_ids;
    {
        SQLite::Statement q(db_.get(), "SELECT show_id FROM show WHERE match_status='matched'");
        while (q.executeStep()) show_ids.push_back(q.getColumn(0).getString());
    }
    {
        SQLite::Statement q(db_.get(), "SELECT movie_id FROM movie WHERE match_status='matched'");
        while (q.executeStep()) movie_ids.push_back(q.getColumn(0).getString());
    }
    std::cout << "[scraper] refresh-all: " << show_ids.size() << " show(s), "
              << movie_ids.size() << " movie(s)\n";

    int refreshed = 0;
    for (const auto& id : show_ids) {
        try { if (refreshMetadata(id, "show")) ++refreshed; }
        catch (const std::exception& e) {
            std::cerr << "[scraper] refresh-all: show " << id << " failed: " << e.what() << "\n";
        }
    }
    for (const auto& id : movie_ids) {
        try { if (refreshMetadata(id, "movie")) ++refreshed; }
        catch (const std::exception& e) {
            std::cerr << "[scraper] refresh-all: movie " << id << " failed: " << e.what() << "\n";
        }
    }
    std::cout << "[scraper] refresh-all done: " << refreshed << "/"
              << (show_ids.size() + movie_ids.size()) << " item(s) refreshed\n";
}

void ScraperManager::runMatch(const std::string& target_id,
                               const std::string& item_type) {
    // Count pending items so the log line gives useful context up front.
    // An item opted out via ANY library it's mapped to is opted out
    // entirely (EXISTS, not a join) — same rule the selection queries below
    // use, so the two stay consistent.
    int pending_shows = 0, pending_movies = 0;
    if (item_type.empty() || item_type == "show") {
        std::string csql = R"(
            SELECT COUNT(*) FROM show s
            WHERE s.match_status IN ('unscraped','uncertain','unmatched')
              AND s.skip_scraping = 0
              AND NOT EXISTS (
                SELECT 1 FROM source_mapping sm
                JOIN media_library ml ON ml.library_id = sm.library_id
                WHERE sm.kairos_id = s.show_id AND sm.item_type = 'show' AND ml.skip_scraping = 1
              )
        )";
        if (!target_id.empty()) csql += " AND s.show_id='" + target_id + "'";
        SQLite::Statement cq(db_.get(), csql);
        if (cq.executeStep()) pending_shows = cq.getColumn(0).getInt();
    }
    if (item_type.empty() || item_type == "movie") {
        std::string csql = R"(
            SELECT COUNT(*) FROM movie m
            WHERE m.match_status IN ('unscraped','uncertain','unmatched')
              AND m.skip_scraping = 0
              AND NOT EXISTS (
                SELECT 1 FROM source_mapping sm
                JOIN media_library ml ON ml.library_id = sm.library_id
                WHERE sm.kairos_id = m.movie_id AND sm.item_type = 'movie' AND ml.skip_scraping = 1
              )
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
            SELECT s.show_id, s.title, COALESCE(s.year,0), s.tmdb_id, s.tvdb_id
            FROM show s
            WHERE s.match_status IN ('unscraped','uncertain','unmatched')
              AND s.skip_scraping = 0
              AND NOT EXISTS (
                SELECT 1 FROM source_mapping sm
                JOIN media_library ml ON ml.library_id = sm.library_id
                WHERE sm.kairos_id = s.show_id AND sm.item_type = 'show' AND ml.skip_scraping = 1
              )
        )";
        if (!target_id.empty()) sql += " AND s.show_id = '" + target_id + "'";

        // Drained into a vector rather than iterating the live cursor — matchShow()
        // can now trigger an auto-merge that deletes a show row (possibly the one
        // the cursor is currently on) via mergeIfDuplicateExternalId().
        struct ShowRow { std::string kairos_id, title, tmdb_id, tvdb_id; int year; };
        std::vector<ShowRow> rows;
        {
            SQLite::Statement q(db_.get(), sql);
            while (q.executeStep()) {
                rows.push_back({ q.getColumn(0).getString(), q.getColumn(1).getString(),
                                  q.getColumn(3).getString(), q.getColumn(4).getString(),
                                  q.getColumn(2).getInt() });
            }
        }
        for (const auto& row : rows) {
            // One row per show now (no per-library join), so matchShow()
            // really is only called once per item per pass — settings from
            // every library the item is mapped to are merged below instead
            // of the query silently picking (and re-picking) just one.
            auto settings = resolveMatchSettings("show", row.kairos_id);
            matchShow(settings, row.kairos_id, row.title, row.year, row.tmdb_id, row.tvdb_id);
        }
    }

    // ── Movies ───────────────────────────────────────────────────────────────
    if (item_type.empty() || item_type == "movie") {
        std::string sql = R"(
            SELECT m.movie_id, m.title, COALESCE(m.year,0), m.tmdb_id, COALESCE(m.file_path,'')
            FROM movie m
            WHERE m.match_status IN ('unscraped','uncertain','unmatched')
              AND m.skip_scraping = 0
              AND NOT EXISTS (
                SELECT 1 FROM source_mapping sm
                JOIN media_library ml ON ml.library_id = sm.library_id
                WHERE sm.kairos_id = m.movie_id AND sm.item_type = 'movie' AND ml.skip_scraping = 1
              )
        )";
        if (!target_id.empty()) sql += " AND m.movie_id = '" + target_id + "'";

        // See the show loop above: drained into a vector since matchMovie() can
        // now delete a movie row (possibly the cursor's current row) via merge.
        struct MovieRow { std::string kairos_id, title, tmdb_id, file_path; int year; };
        std::vector<MovieRow> rows;
        {
            SQLite::Statement q(db_.get(), sql);
            while (q.executeStep()) {
                rows.push_back({ q.getColumn(0).getString(), q.getColumn(1).getString(),
                                  q.getColumn(3).getString(), q.getColumn(4).getString(),
                                  q.getColumn(2).getInt() });
            }
        }
        for (const auto& row : rows) {
            auto settings = resolveMatchSettings("movie", row.kairos_id);
            matchMovie(settings, row.kairos_id, row.title, row.year, row.tmdb_id, row.file_path);
        }
    }
    std::cout << "[scraper] match complete ("
              << elapsedMs(t_match_start, std::chrono::steady_clock::now()) << "ms)\n";
}

// ── Per-item matching ─────────────────────────────────────────────────────────

ScraperManager::MatchSettings
ScraperManager::resolveMatchSettings(const std::string& item_type, const std::string& kairos_id) const {
    MatchSettings out;

    std::vector<std::string> library_ids;
    {
        SQLite::Statement q(db_.get(), R"(
            SELECT DISTINCT library_id FROM source_mapping
            WHERE kairos_id = ? AND item_type = ? AND library_id IS NOT NULL
        )");
        q.bind(1, kairos_id);
        q.bind(2, item_type);
        while (q.executeStep()) library_ids.push_back(q.getColumn(0).getString());
    }

    std::set<std::string> seen_priority;
    for (const auto& lib_id : library_ids) {
        if (auto lib = sourceRepo().getLibrary(lib_id)) {
            if (!lib->preferred_language.empty()) out.preferred_languages.insert(lib->preferred_language);
            if (lib->include_anidb) out.include_anidb = true;
        }
        for (const auto& src : sourceRepo().getScraperPriority(lib_id, item_type))
            if (seen_priority.insert(src).second) out.priority.push_back(src);
    }

    if (out.preferred_languages.size() > 1) {
        std::string joined;
        for (const auto& lang : out.preferred_languages) joined += (joined.empty() ? "" : ", ") + lang;
        std::cerr << "[scraper] settings conflict: " << item_type << " " << kairos_id
                  << " is mapped to libraries with differing preferred_language (" << joined
                  << ") — scoring credits a match against any of them, none is enforced\n";
    }

    return out;
}

void ScraperManager::matchShow(const MatchSettings& settings, const std::string& kairos_id, const std::string& title,
                                int year, const std::string& tmdb_id,
                                const std::string& tvdb_id) {
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
            std::string stripped = cleanFolderTitle(folder);
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
            const std::string src = !tmdb_id.empty() ? "tmdb" : "tvdb";
            const std::string eid = !tmdb_id.empty() ? tmdb_id : tvdb_id;
            auto dup = mergeIfDuplicateExternalId("show", kairos_id, src, eid);
            if (!dup.merged_into_kairos_id.empty()) return;
            linkExternalId("show", kairos_id, src, eid, /*promote_to_primary=*/true);
            setMatchStatus("show", kairos_id, "matched", 1.0);
            // Auto-match only flips status/score — local metadata (release_date,
            // overview, genres, ...) still needs fetching so shelves that sort on
            // it (e.g. Recently Released) see this item without waiting on a
            // human review-queue accept. match_confirmed stays unset, so
            // writeback-to-source still gates on an explicit human accept.
            refreshMetadata(kairos_id, "show");
            return;
        }
        // Folder mismatch: ignore the provided ID and search with the folder name.
    }

    if (search_title.empty()) {
        std::cout << "[scraper]   (no title, id=" << kairos_id << ") → unmatched\n";
        setMatchStatus("show", kairos_id, "unmatched", 0.0);
        return;
    }

    const std::vector<std::string>& priority = settings.priority;

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
            if (cfg.source == source && settings.preferred_languages.count(cfg.language)) {
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
        if (anidb_ && (settings.include_anidb || std::find(priority.begin(), priority.end(), "anidb") != priority.end())) {
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
        auto dup = mergeIfDuplicateExternalId("show", kairos_id, best_c.source, best_c.ext_id);
        if (!dup.merged_into_kairos_id.empty()) return;
        linkExternalId("show", kairos_id, best_c.source, best_c.ext_id, /*promote_to_primary=*/true);
        setMatchStatus("show", kairos_id, "matched", best);
        // See the trusted-ID branch above for why this is needed here too.
        refreshMetadata(kairos_id, "show");
    } else {
        std::cout << "[scraper]   \"" << search_title << "\" → uncertain"
                  << " (" << std::fixed << std::setprecision(2) << best << ")\n";
        setMatchStatus("show", kairos_id, "uncertain", best);
    }
}

void ScraperManager::matchMovie(const MatchSettings& settings, const std::string& kairos_id, const std::string& title,
                                 int year, const std::string& tmdb_id,
                                 const std::string& file_path) {
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
            std::string folder = cleanFolderTitle(fs::path(mapped).parent_path().filename().string());
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
        auto dup = mergeIfDuplicateExternalId("movie", kairos_id, "tmdb", tmdb_id);
        if (!dup.merged_into_kairos_id.empty()) return;
        linkExternalId("movie", kairos_id, "tmdb", tmdb_id, /*promote_to_primary=*/true);
        setMatchStatus("movie", kairos_id, "matched", 1.0);
        // See matchShow()'s trusted-ID branch for why this fetch is needed —
        // same local-metadata gap (release_date etc.) otherwise.
        refreshMetadata(kairos_id, "movie");
        return;
    }

    // Search path: no trusted ID, or folder mismatch disqualified the provided ID.
    if (search_title.empty()) {
        std::cout << "[scraper]   (no title, id=" << kairos_id << ") → unmatched\n";
        setMatchStatus("movie", kairos_id, "unmatched", 0.0);
        return;
    }

    const std::vector<std::string>& priority = settings.priority;

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
            if (cfg.source == source && settings.preferred_languages.count(cfg.language)) {
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
        if (anidb_ && (settings.include_anidb || std::find(priority.begin(), priority.end(), "anidb") != priority.end())) {
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
        auto dup = mergeIfDuplicateExternalId("movie", kairos_id, best_c.source, best_c.ext_id);
        if (!dup.merged_into_kairos_id.empty()) return;
        linkExternalId("movie", kairos_id, best_c.source, best_c.ext_id, /*promote_to_primary=*/true);
        setMatchStatus("movie", kairos_id, "matched", best);
        // See the trusted-ID branch above for why this is needed here too.
        refreshMetadata(kairos_id, "movie");
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

// ── Local AniDB poster persistence ──────────────────────────────────────────

void ScraperManager::persistAnidbThumbLocally(const std::string& item_type, const std::string& kairos_id) {
    if (!anidb_ || !getSettings().anidb_download_posters) return;

    const std::string table = (item_type == "show") ? "show" : "movie";
    const std::string idcol = table + "_id";

    std::string thumb;
    bool locked = false;
    try {
        SQLite::Statement q(db_.get(), "SELECT thumb, locked FROM " + table + " WHERE " + idcol + " = ?");
        q.bind(1, kairos_id);
        if (!q.executeStep()) return;
        thumb  = q.getColumn(0).getString();
        locked = q.getColumn(1).getInt() != 0;
    } catch (...) { return; }

    // Only ever downloads a genuine AniDB CDN URL — never a "local:" path
    // already persisted by a previous call, and never a poster another
    // source (tmdb/tvdb) supplied for the same item.
    if (locked || thumb.empty() || thumb.find("anidb.net") == std::string::npos) return;

    std::string folder;
    if (item_type == "show") {
        folder = ContentRepository(db_).getShowFolderPath(kairos_id).value_or("");
    } else {
        if (auto d = ContentRepository(db_).getMovieDetail(kairos_id))
            folder = ContentRepository::parentDir(d->file_path);
    }
    if (folder.empty()) return;

    std::string bytes = anidb_->fetchImageBytes(thumb);
    if (bytes.empty()) return;

    std::error_code ec;
    fs::path dest = fs::path(folder) / "aniThumb.jpg";
    try {
        std::ofstream f(dest, std::ios::binary);
        if (!f) return;
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!f) return;
    } catch (...) { return; }

    try {
        SQLite::Statement upd(db_.get(), "UPDATE " + table + " SET thumb = ? WHERE " + idcol + " = ?");
        upd.bind(1, "local:" + dest.string());
        upd.bind(2, kairos_id);
        upd.exec();
    } catch (...) {}
}

// ── Accept / reject ───────────────────────────────────────────────────────────

AcceptResult ScraperManager::acceptCandidate(const std::string& candidate_id) {
    // Read candidate
    SQLite::Statement rd(db_.get(),
        "SELECT item_type, kairos_id, source, external_id, score "
        "FROM item_match_candidate WHERE candidate_id=?");
    rd.bind(1, candidate_id);
    if (!rd.executeStep()) return {};

    std::string item_type   = rd.getColumn(0).getString();
    std::string kairos_id   = rd.getColumn(1).getString();
    std::string source      = rd.getColumn(2).getString();
    std::string external_id = rd.getColumn(3).getString();
    double score            = rd.getColumn(4).getDouble();

    // Must run before any transaction opens below — mergeShowInto/mergeMovieInto
    // each open their own SQLite::Transaction, and nested transactions aren't
    // supported. If this item is really a duplicate of an already-matched item,
    // fold it in now and stop; nothing past this point should touch kairos_id.
    auto dup = mergeIfDuplicateExternalId(item_type, kairos_id, source, external_id);
    if (!dup.merged_into_kairos_id.empty()) {
        return { true, item_type, dup.merged_into_kairos_id, dup.merged_into_title, dup.folder_mismatch };
    }

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
                    applyShowMetadata(db_.get(), kairos_id, *show);
                    persistAnidbThumbLocally(item_type, kairos_id);
                }
            } else if (item_type == "movie") {
                auto movie = anidb_->fetchMovie(external_id, language);
                if (movie) {
                    applyMovieMetadata(db_.get(), kairos_id, *movie);
                    persistAnidbThumbLocally(item_type, kairos_id);
                }
            }
        }
        if (source == "tmdb" && tmdb_) {
            if (item_type == "show") {
                auto show = tmdb_->fetchShow(external_id, language);
                if (show) {
                    linkExternalId(item_type, kairos_id, "tvdb", show->tvdb_id, /*promote_to_primary=*/false);
                    linkExternalId(item_type, kairos_id, "imdb", show->imdb_id, /*promote_to_primary=*/false);
                    applyShowMetadata(db_.get(), kairos_id, *show);
                }
            } else if (item_type == "movie") {
                auto movie = tmdb_->fetchMovie(external_id, language);
                if (movie) {
                    linkExternalId(item_type, kairos_id, "imdb", movie->imdb_id, /*promote_to_primary=*/false);
                    applyMovieMetadata(db_.get(), kairos_id, *movie);
                }
            }
        }
        if (source == "tvdb" && tvdb_) {
            if (item_type == "show") {
                auto show = tvdb_->fetchShow(external_id, language);
                if (show) {
                    linkExternalId(item_type, kairos_id, "tmdb", show->tmdb_id, /*promote_to_primary=*/false);
                    linkExternalId(item_type, kairos_id, "imdb", show->imdb_id, /*promote_to_primary=*/false);
                    applyShowMetadata(db_.get(), kairos_id, *show);
                }
            } else if (item_type == "movie") {
                auto movie = tvdb_->fetchMovie(external_id, language);
                if (movie) applyMovieMetadata(db_.get(), kairos_id, *movie);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[scraper] metadata fetch/apply failed for " << kairos_id << " (" << source << "): " << e.what() << "\n";
    }
    return { true, item_type, "", "", false };
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

AcceptResult ScraperManager::manualMatch(const std::string& kairos_id,
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
                        applyShowMetadata(db_.get(), kairos_id, *show);
                        any_success = true;
                    }
                } else if (item_type == "movie") {
                    auto movie = tmdb_->fetchMovie(id.external_id, language);
                    if (movie) {
                        linkExternalId(item_type, kairos_id, "imdb", movie->imdb_id, /*promote_to_primary=*/false);
                        applyMovieMetadata(db_.get(), kairos_id, *movie);
                        any_success = true;
                    }
                }
            } else if (id.source == "tvdb" && tvdb_) {
                if (item_type == "show") {
                    auto show = tvdb_->fetchShow(id.external_id, language);
                    if (show) {
                        linkExternalId(item_type, kairos_id, "tmdb", show->tmdb_id, /*promote_to_primary=*/false);
                        linkExternalId(item_type, kairos_id, "imdb", show->imdb_id, /*promote_to_primary=*/false);
                        applyShowMetadata(db_.get(), kairos_id, *show);
                        any_success = true;
                    }
                } else if (item_type == "movie") {
                    auto movie = tvdb_->fetchMovie(id.external_id, language);
                    if (movie) {
                        linkExternalId(item_type, kairos_id, "tmdb", movie->tmdb_id, /*promote_to_primary=*/false);
                        linkExternalId(item_type, kairos_id, "imdb", movie->imdb_id, /*promote_to_primary=*/false);
                        applyMovieMetadata(db_.get(), kairos_id, *movie);
                        any_success = true;
                    }
                }
            } else if (id.source == "anidb" && anidb_) {
                if (item_type == "show") {
                    auto show = anidb_->fetchShow(id.external_id, language);
                    if (show) {
                        linkExternalId(item_type, kairos_id, "tvdb", show->tvdb_id, /*promote_to_primary=*/false);
                        applyShowMetadata(db_.get(), kairos_id, *show);
                        persistAnidbThumbLocally(item_type, kairos_id);
                        any_success = true;
                    }
                } else if (item_type == "movie") {
                    auto movie = anidb_->fetchMovie(id.external_id, language);
                    if (movie) {
                        applyMovieMetadata(db_.get(), kairos_id, *movie);
                        persistAnidbThumbLocally(item_type, kairos_id);
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

    // Text search. Each scraper is capped to its top matches (already
    // relevance-sorted by the scraper itself) — AniDB posters in particular
    // require a rate-limited per-title detail fetch, so an uncapped list of
    // loose matches can serialize tens of seconds of blocked poster requests.
    constexpr size_t kMaxResultsPerScraper = 10;
    auto capped = [](auto items) {
        if (items.size() > kMaxResultsPerScraper) items.resize(kMaxResultsPerScraper);
        return items;
    };

    if (content_type == "show" || content_type.empty()) {
        if (tmdb_) for (auto& s : capped(tmdb_->searchShows(query))) addShow("tmdb", s);
        if (tvdb_) for (auto& s : capped(tvdb_->searchShows(query))) addShow("tvdb", s);
        // AniDB's title dump has no show/movie distinction (real type only
        // comes from the full per-title detail fetch), so an "all" search
        // queries it here only, not again in the movie branch below —
        // querying twice doubled its already rate-limited poster fetches and
        // produced a duplicate tile for every match. Ambiguous matches
        // default to "show".
        if (anidb_ && content_type != "movie")
            for (auto& s : capped(anidb_->searchShows(query))) addShow("anidb", s);
    }
    if (content_type == "movie" || content_type.empty()) {
        if (tmdb_) for (auto& m : capped(tmdb_->searchMovies(query))) addMovie("tmdb", m);
        if (tvdb_) for (auto& m : capped(tvdb_->searchMovies(query))) addMovie("tvdb", m);
        if (anidb_ && content_type == "movie")
            for (auto& m : capped(anidb_->searchMovies(query))) addMovie("anidb", m);
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

// ── Show specials linking ───────────────────────────────────────────────────

std::vector<std::pair<std::string, std::string>>
ScraperManager::confirmedShowSourceIds(const std::string& show_id) const {
    std::vector<std::pair<std::string, std::string>> out;
    std::set<std::string> seen;

    // Richest source: populated once a human confirms via acceptCandidate().
    for (const auto& eid : getExternalIds(show_id, "show"))
        if (!eid.external_id.empty() && seen.insert(eid.source).second)
            out.push_back({eid.source, eid.external_id});

    // Fallback: an auto-matched item that hasn't been human-confirmed yet
    // never reaches acceptCandidate(), so item_external_id is empty — the
    // winning id only lives on the item_match_candidate row itself.
    {
        SQLite::Statement q(db_.get(),
            "SELECT source, external_id FROM item_match_candidate "
            "WHERE item_type='show' AND kairos_id=? AND accepted=1");
        q.bind(1, show_id);
        while (q.executeStep()) {
            std::string src = q.getColumn(0).getString();
            std::string ext = q.getColumn(1).getString();
            if (!ext.empty() && seen.insert(src).second) out.push_back({src, ext});
        }
    }

    // Fallback: the trusted-ID short-circuit in matchShow() sets match_status
    // directly from the show's own tvdb_id/tmdb_id columns without ever
    // querying a scraper or touching either table above.
    {
        SQLite::Statement q(db_.get(), "SELECT tvdb_id, tmdb_id FROM show WHERE show_id=?");
        q.bind(1, show_id);
        if (q.executeStep()) {
            std::string tvdb = q.getColumn(0).getString();
            std::string tmdb = q.getColumn(1).getString();
            if (!tvdb.empty() && seen.insert("tvdb").second) out.push_back({"tvdb", tvdb});
            if (!tmdb.empty() && seen.insert("tmdb").second) out.push_back({"tmdb", tmdb});
        }
    }
    return out;
}

std::vector<SpecialCandidate> ScraperManager::scanSpecialsForShow(const std::string& show_id) {
    std::string show_title;
    {
        SQLite::Statement q(db_.get(), "SELECT title FROM show WHERE show_id=?");
        q.bind(1, show_id);
        if (!q.executeStep()) return {};
        show_title = q.getColumn(0).getString();
    }

    auto source_ids = confirmedShowSourceIds(show_id);

    // Query every confirmed source, not just the top-priority one, and keep
    // only season-0 (specials) entries from each.
    struct SourceSpecials { std::string source; std::vector<Episode> specials; };
    std::vector<SourceSpecials> per_source;
    for (const auto& [source, ext_id] : source_ids) {
        std::vector<Episode> eps;
        if      (source == "tmdb"  && tmdb_)  eps = tmdb_->fetchEpisodes(ext_id);
        else if (source == "tvdb"  && tvdb_)  eps = tvdb_->fetchEpisodes(ext_id);
        else if (source == "anidb" && anidb_) eps = anidb_->fetchEpisodes(ext_id);
        else continue;

        std::vector<Episode> specials;
        for (auto& ep : eps) if (ep.season == 0) specials.push_back(std::move(ep));
        if (!specials.empty())
            per_source.push_back({source, std::move(specials)});
    }

    // Every scraper either isn't reachable or genuinely has nothing to say
    // right now — leave existing candidates untouched rather than risk
    // wiping pending review rows because of a transient failure we can't
    // tell apart from "no specials" (fetchEpisodes returns {} either way).
    if (per_source.empty()) {
        std::cout << "[scraper] specials scan: \"" << show_title << "\" — no source data, skipping\n";
        return getSpecialCandidates(show_id);
    }

    // Merge duplicates across sources into one canonical list. per_source is
    // already in confirmedShowSourceIds' priority order, so the first source
    // to report a given special "wins" its title/overview/number; later
    // sources only fill in blanks.
    struct Merged {
        std::string title, overview, air_date, thumb, source, source_episode_id;
        int number = 0;
    };
    std::vector<Merged> merged;
    for (const auto& src : per_source) {
        for (const auto& ep : src.specials) {
            bool found = false;
            for (auto& m : merged) {
                if (sameSpecial(ep.title, ep.air_date, m.title, m.air_date)) {
                    if (m.overview.empty())  m.overview  = ep.overview;
                    if (m.thumb.empty())     m.thumb     = ep.thumb;
                    if (m.air_date.empty())  m.air_date  = ep.air_date;
                    found = true;
                    break;
                }
            }
            if (!found) {
                merged.push_back({ep.title, ep.overview, ep.air_date, ep.thumb,
                                   src.source, ep.episode_id, ep.episode});
            }
        }
    }

    // Specials that already have a real on-disk episode row need no link.
    std::set<int> already_filed;
    {
        SQLite::Statement q(db_.get(),
            "SELECT episode FROM episode WHERE show_id=? AND season=0 AND file_path != ''");
        q.bind(1, show_id);
        while (q.executeStep()) already_filed.insert(q.getColumn(0).getInt());
    }

    struct MovieRow { std::string id, title; int year; };
    std::vector<MovieRow> movies;
    {
        SQLite::Statement q(db_.get(), R"(
            SELECT movie_id, title, COALESCE(year,0) FROM movie
            WHERE skip_scraping = 0
              AND movie_id NOT IN (SELECT linked_movie_id FROM episode WHERE linked_movie_id IS NOT NULL)
        )");
        while (q.executeStep())
            movies.push_back({q.getColumn(0).getString(), q.getColumn(1).getString(), q.getColumn(2).getInt()});
    }

    constexpr double kFloor         = 0.35; // keeps the list signal-heavy; linking always needs a human accept
    constexpr size_t kMaxPerSpecial = 5;

    std::set<int> reported_numbers;
    {
        SQLite::Transaction txn(db_.get());
        for (const auto& m : merged) {
            if (already_filed.count(m.number)) continue;
            reported_numbers.insert(m.number);

            int special_year = 0;
            if (m.air_date.size() >= 4) {
                try { special_year = std::stoi(m.air_date.substr(0, 4)); } catch (...) {}
            }

            std::vector<std::pair<double, const MovieRow*>> scored;
            for (const auto& mv : movies) {
                double sc = computeScore(m.title, special_year, mv.title, mv.year);
                if (sc >= kFloor) scored.push_back({sc, &mv});
            }
            std::sort(scored.begin(), scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            if (scored.size() > kMaxPerSpecial) scored.resize(kMaxPerSpecial);

            for (const auto& [score, mv] : scored) {
                std::string cid = "special:" + show_id + ":" + std::to_string(m.number) + ":" + mv->id;
                SQLite::Statement up(db_.get(), R"(
                    INSERT INTO special_link_candidate
                        (candidate_id, show_id, episode_number, special_title, special_overview,
                         special_air_date, special_thumb, source, source_episode_id, movie_id, score)
                    VALUES (?,?,?,?,?,?,?,?,?,?,?)
                    ON CONFLICT(show_id, episode_number, movie_id) DO UPDATE SET
                        special_title     = excluded.special_title,
                        special_overview  = excluded.special_overview,
                        special_air_date  = excluded.special_air_date,
                        special_thumb     = excluded.special_thumb,
                        source            = excluded.source,
                        source_episode_id = excluded.source_episode_id,
                        score             = excluded.score
                    WHERE accepted IS NULL
                )");
                up.bind(1, cid); up.bind(2, show_id); up.bind(3, m.number);
                up.bind(4, m.title); up.bind(5, m.overview); up.bind(6, m.air_date);
                up.bind(7, m.thumb); up.bind(8, m.source); up.bind(9, m.source_episode_id);
                up.bind(10, mv->id); up.bind(11, score);
                up.exec();
            }
        }

        // A pending candidate for an episode-number this scan no longer
        // reports has nothing left to preserve — decided (accepted/rejected)
        // rows are untouched regardless.
        std::vector<int> stale;
        {
            SQLite::Statement q(db_.get(),
                "SELECT DISTINCT episode_number FROM special_link_candidate "
                "WHERE show_id=? AND accepted IS NULL");
            q.bind(1, show_id);
            while (q.executeStep()) {
                int n = q.getColumn(0).getInt();
                if (!reported_numbers.count(n)) stale.push_back(n);
            }
        }
        for (int n : stale) {
            SQLite::Statement d(db_.get(),
                "DELETE FROM special_link_candidate WHERE show_id=? AND episode_number=? AND accepted IS NULL");
            d.bind(1, show_id); d.bind(2, n); d.exec();
        }

        txn.commit();
    }

    std::cout << "[scraper] specials scan: \"" << show_title << "\" — "
              << merged.size() << " special(s) across " << per_source.size()
              << " source(s)\n";

    return getSpecialCandidates(show_id);
}

std::vector<SpecialCandidate> ScraperManager::getSpecialCandidates(const std::string& show_id) const {
    std::vector<SpecialCandidate> out;
    SQLite::Statement q(db_.get(), R"(
        SELECT c.candidate_id, c.episode_number, c.special_title, c.special_overview,
               c.special_air_date, c.special_thumb, c.source, c.source_episode_id,
               c.movie_id, m.title, COALESCE(m.year,0), c.score, COALESCE(c.accepted,-1)
        FROM special_link_candidate c
        JOIN movie m ON m.movie_id = c.movie_id
        WHERE c.show_id = ?
        ORDER BY c.episode_number ASC, c.score DESC
    )");
    q.bind(1, show_id);
    while (q.executeStep()) {
        SpecialCandidate c;
        c.candidate_id      = q.getColumn(0).getString();
        c.show_id           = show_id;
        c.episode_number    = q.getColumn(1).getInt();
        c.special_title     = q.getColumn(2).getString();
        c.special_overview  = q.getColumn(3).getString();
        c.special_air_date  = q.getColumn(4).getString();
        c.special_thumb     = q.getColumn(5).getString();
        c.source            = q.getColumn(6).getString();
        c.source_episode_id = q.getColumn(7).getString();
        c.movie_id          = q.getColumn(8).getString();
        c.movie_title       = q.getColumn(9).getString();
        c.movie_year        = q.getColumn(10).getInt();
        c.score              = q.getColumn(11).getDouble();
        c.accepted           = q.getColumn(12).getInt();
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<LinkedSpecial> ScraperManager::getLinkedSpecials(const std::string& show_id) const {
    std::vector<LinkedSpecial> out;
    SQLite::Statement q(db_.get(), R"(
        SELECT e.episode_id, e.episode, e.title, e.linked_movie_id, m.title
        FROM episode e
        JOIN movie m ON m.movie_id = e.linked_movie_id
        WHERE e.show_id = ? AND e.season = 0 AND e.linked_movie_id IS NOT NULL
        ORDER BY e.episode ASC
    )");
    q.bind(1, show_id);
    while (q.executeStep()) {
        out.push_back({
            q.getColumn(0).getString(),
            q.getColumn(1).getInt(),
            q.getColumn(2).getString(),
            q.getColumn(3).getString(),
            q.getColumn(4).getString(),
        });
    }
    return out;
}

bool ScraperManager::acceptSpecialCandidate(const std::string& candidate_id) {
    std::string show_id, title, overview, air_date, thumb, movie_id;
    int number = 0;
    {
        SQLite::Statement q(db_.get(), R"(
            SELECT show_id, episode_number, special_title, special_overview,
                   special_air_date, special_thumb, movie_id
            FROM special_link_candidate WHERE candidate_id = ?
        )");
        q.bind(1, candidate_id);
        if (!q.executeStep()) return false;
        show_id  = q.getColumn(0).getString();
        number   = q.getColumn(1).getInt();
        title    = q.getColumn(2).getString();
        overview = q.getColumn(3).getString();
        air_date = q.getColumn(4).getString();
        thumb    = q.getColumn(5).getString();
        movie_id = q.getColumn(6).getString();
    }

    int64_t movie_duration_ms = 0;
    std::string movie_thumb;
    {
        SQLite::Statement q(db_.get(), "SELECT duration_ms, thumb FROM movie WHERE movie_id=?");
        q.bind(1, movie_id);
        if (q.executeStep()) {
            movie_duration_ms = q.getColumn(0).getInt64();
            movie_thumb       = q.getColumn(1).getString();
        }
    }
    const std::string effective_thumb = !thumb.empty() ? thumb : movie_thumb;

    SQLite::Transaction txn(db_.get());

    // An existing (show_id, season=0, episode=number) row covers re-accepting
    // after a reject-then-reconsider — update it in place instead of
    // colliding on a fresh id.
    std::string episode_id;
    {
        SQLite::Statement q(db_.get(),
            "SELECT episode_id FROM episode WHERE show_id=? AND season=0 AND episode=?");
        q.bind(1, show_id); q.bind(2, number);
        if (q.executeStep()) episode_id = q.getColumn(0).getString();
    }

    if (episode_id.empty()) {
        episode_id = "special:" + show_id + ":" + std::to_string(number);
        SQLite::Statement ins(db_.get(), R"(
            INSERT INTO episode (episode_id, show_id, season, episode, title, file_path,
                                  duration_ms, overview, air_date, thumb, linked_movie_id, locked)
            VALUES (?, ?, 0, ?, ?, '', ?, ?, ?, ?, ?, 1)
        )");
        ins.bind(1, episode_id); ins.bind(2, show_id); ins.bind(3, number);
        ins.bind(4, title); ins.bind(5, movie_duration_ms); ins.bind(6, overview);
        ins.bind(7, air_date); ins.bind(8, effective_thumb); ins.bind(9, movie_id);
        ins.exec();
    } else {
        SQLite::Statement upd(db_.get(), R"(
            UPDATE episode SET title=?, file_path='', duration_ms=?, overview=?, air_date=?,
                                thumb=?, linked_movie_id=?, locked=1
            WHERE episode_id=?
        )");
        upd.bind(1, title); upd.bind(2, movie_duration_ms); upd.bind(3, overview);
        upd.bind(4, air_date); upd.bind(5, effective_thumb); upd.bind(6, movie_id);
        upd.bind(7, episode_id);
        upd.exec();
    }

    {
        SQLite::Statement u(db_.get(),
            "UPDATE special_link_candidate SET accepted=1, decided_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
            "WHERE candidate_id=?");
        u.bind(1, candidate_id); u.exec();
    }
    // Only one movie can back one special — supersede sibling pending
    // candidates for the same (show, episode-number) without deleting them.
    {
        SQLite::Statement u(db_.get(), R"(
            UPDATE special_link_candidate
            SET accepted=0, decided_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
            WHERE show_id=? AND episode_number=? AND candidate_id != ? AND accepted IS NULL
        )");
        u.bind(1, show_id); u.bind(2, number); u.bind(3, candidate_id);
        u.exec();
    }

    txn.commit();
    return true;
}

bool ScraperManager::rejectSpecialCandidate(const std::string& candidate_id) {
    SQLite::Statement rd(db_.get(), "SELECT 1 FROM special_link_candidate WHERE candidate_id=?");
    rd.bind(1, candidate_id);
    if (!rd.executeStep()) return false;

    SQLite::Statement u(db_.get(),
        "UPDATE special_link_candidate SET accepted=0, decided_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
        "WHERE candidate_id=?");
    u.bind(1, candidate_id);
    u.exec();
    return true;
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
