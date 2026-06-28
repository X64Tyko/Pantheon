#include "ScraperManager.h"
#include "TmdbScraper.h"
#include "TvdbScraper.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
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

// Minimum folder/title similarity to trust a Plex-supplied external ID.
constexpr double kFolderTitleThreshold = 0.80;

} // namespace

// ── Constructor ───────────────────────────────────────────────────────────────

ScraperManager::ScraperManager(Database& db, ConfStore& conf)
    : db_(db), conf_(conf)
{
    buildScrapers();
}

ScraperManager::~ScraperManager() = default;

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
    s.match_threshold = std::stod(readKey("match_threshold", "1.0"));

    for (const auto* src : { "tmdb", "tvdb" }) {
        ScraperConfig c;
        c.source   = src;
        c.api_key  = readKey(configKey(src, "api_key"),  "");
        c.language = readKey(configKey(src, "language"), src == std::string("tvdb") ? "eng" : "en");
        c.enabled  = readKey(configKey(src, "enabled"),  "0") == "1";
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
        if (c.source == "tvdb") writeKey(configKey(c.source, "pin"), c.pin);
    }
    buildScrapers();
}

void ScraperManager::buildScrapers() {
    auto s = getSettings();
    tmdb_.reset();
    tvdb_.reset();
    for (const auto& c : s.configs) {
        if (!c.enabled || c.api_key.empty()) continue;
        if (c.source == "tmdb") tmdb_ = std::make_unique<TmdbScraper>(c.api_key, c.language);
        if (c.source == "tvdb") tvdb_ = std::make_unique<TvdbScraper>(c.api_key, c.language, c.pin);
    }
}

double ScraperManager::threshold() const {
    SQLite::Statement q(db_.get(), "SELECT value FROM app_config WHERE key='match_threshold'");
    if (q.executeStep()) {
        try { return std::stod(q.getColumn(0).getString()); } catch (...) {}
    }
    return 1.0;
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
    int pending_shows = 0, pending_movies = 0;
    if (item_type.empty() || item_type == "show") {
        std::string csql = "SELECT COUNT(*) FROM show WHERE match_status IN ('unscraped','uncertain','unmatched')";
        if (!target_id.empty()) csql += " AND show_id='" + target_id + "'";
        SQLite::Statement cq(db_.get(), csql);
        if (cq.executeStep()) pending_shows = cq.getColumn(0).getInt();
    }
    if (item_type.empty() || item_type == "movie") {
        std::string csql = "SELECT COUNT(*) FROM movie WHERE match_status IN ('unscraped','uncertain','unmatched')";
        if (!target_id.empty()) csql += " AND movie_id='" + target_id + "'";
        SQLite::Statement cq(db_.get(), csql);
        if (cq.executeStep()) pending_movies = cq.getColumn(0).getInt();
    }

    if (pending_shows + pending_movies == 0) {
        std::cout << "[scraper] match: nothing pending\n";
        return;
    }
    std::cout << "[scraper] match: " << pending_shows << " show(s), "
              << pending_movies << " movie(s)\n";

    // ── Shows ────────────────────────────────────────────────────────────────
    if (item_type.empty() || item_type == "show") {
        std::string sql = R"(
            SELECT show_id, title, COALESCE(year,0), tmdb_id, tvdb_id
            FROM show WHERE match_status IN ('unscraped','uncertain','unmatched')
        )";
        if (!target_id.empty()) sql += " AND show_id = '" + target_id + "'";

        SQLite::Statement q(db_.get(), sql);
        while (q.executeStep()) {
            matchShow(q.getColumn(0).getString(),
                      q.getColumn(1).getString(),
                      q.getColumn(2).getInt(),
                      q.getColumn(3).getString(),
                      q.getColumn(4).getString());
        }
    }

    // ── Movies ───────────────────────────────────────────────────────────────
    if (item_type.empty() || item_type == "movie") {
        std::string sql = R"(
            SELECT movie_id, title, COALESCE(year,0), tmdb_id, COALESCE(file_path,'')
            FROM movie WHERE match_status IN ('unscraped','uncertain','unmatched')
        )";
        if (!target_id.empty()) sql += " AND movie_id = '" + target_id + "'";

        SQLite::Statement q(db_.get(), sql);
        while (q.executeStep()) {
            matchMovie(q.getColumn(0).getString(),
                       q.getColumn(1).getString(),
                       q.getColumn(2).getInt(),
                       q.getColumn(3).getString(),
                       q.getColumn(4).getString());
        }
    }
    std::cout << "[scraper] match complete\n";
}

// ── Per-item matching ─────────────────────────────────────────────────────────

void ScraperManager::matchShow(const std::string& kairos_id, const std::string& title,
                                int year, const std::string& tmdb_id,
                                const std::string& tvdb_id) {
    // Items with existing external IDs (set by Plex/Jellyfin) are trusted for the
    // metadata match, but we still validate that the episode files actually resolve
    // to a real folder on disk after path mapping.
    if (!tmdb_id.empty() || !tvdb_id.empty()) {
        bool has_paths  = false;
        bool path_ok    = false;
        bool name_ok    = true;
        SQLite::Statement ep(db_.get(),
            "SELECT file_path FROM episode WHERE show_id = ? AND file_path != '' LIMIT 5");
        ep.bind(1, kairos_id);
        while (ep.executeStep() && !path_ok) {
            has_paths = true;
            std::string mapped = conf_.applyPathMap(ep.getColumn(0).getString());
            if (!folderExists(mapped)) continue;
            path_ok = true;
            std::string folder = extractShowFolder(fs::path(mapped));
            double sim = titleSimilarity(title, stripFolderYear(folder));
            if (sim < kFolderTitleThreshold) {
                name_ok = false;
                std::cout << "[scraper]   \"" << title << "\" → uncertain"
                          << " (folder: \"" << folder << "\", sim="
                          << std::fixed << std::setprecision(2) << sim << ")\n";
            }
        }
        auto storePlexShowCandidate = [&]() {
            std::string src = !tmdb_id.empty() ? "tmdb" : "tvdb";
            std::string eid = !tmdb_id.empty() ? tmdb_id : tvdb_id;
            storeCandidate("show", kairos_id, src, eid, title, year, 1.0, "", "");
        };
        if (has_paths && !path_ok) {
            std::cout << "[scraper]   \"" << title << "\" → uncertain (path missing)\n";
            storePlexShowCandidate();
            setMatchStatus("show", kairos_id, "uncertain", 1.0);
        } else if (!name_ok) {
            storePlexShowCandidate();
            setMatchStatus("show", kairos_id, "uncertain", 1.0);
        } else {
            std::cout << "[scraper]   \"" << title << "\" → matched\n";
            setMatchStatus("show", kairos_id, "matched", 1.0);
        }
        return;
    }

    // No external IDs → search scrapers
    if (title.empty()) {
        std::cout << "[scraper]   (no title, id=" << kairos_id << ") → unmatched\n";
        setMatchStatus("show", kairos_id, "unmatched", 0.0);
        return;
    }

    struct Cand { std::string source, ext_id, cand_title, poster, overview; int cand_year; double score; };
    std::vector<Cand> candidates;

    auto collect = [&](const std::string& source, std::vector<Show> results) {
        for (auto& r : results) {
            double sc = computeScore(title, year, r.title,
                                     r.year.has_value() ? r.year.value() : 0);
            std::string ext = (source == "tmdb") ? r.tmdb_id : r.tvdb_id;
            if (ext.empty()) continue;
            candidates.push_back({ source, ext, r.title, r.thumb, r.overview,
                                   r.year.has_value() ? r.year.value() : 0, sc });
        }
    };

    if (tmdb_) collect("tmdb", tmdb_->searchShows(title, year));
    if (tvdb_) collect("tvdb", tvdb_->searchShows(title, year));

    double best = 0.0;
    for (const auto& c : candidates) {
        storeCandidate("show", kairos_id, c.source, c.ext_id,
                        c.cand_title, c.cand_year, c.score, c.poster, c.overview);
        if (c.score > best) best = c.score;
    }

    if (candidates.empty()) {
        std::cout << "[scraper]   \"" << title << "\" → unmatched\n";
        setMatchStatus("show", kairos_id, "unmatched", 0.0);
    } else if (best >= threshold()) {
        const auto& best_c = *std::max_element(candidates.begin(), candidates.end(),
            [](const Cand& a, const Cand& b){ return a.score < b.score; });
        std::cout << "[scraper]   \"" << title << "\" → matched"
                  << " (" << best_c.source << ", "
                  << std::fixed << std::setprecision(2) << best << ")\n";
        std::string cid = candidateKey("show", kairos_id, best_c.source, best_c.ext_id);
        SQLite::Statement upd(db_.get(),
            "UPDATE item_match_candidate SET accepted=1 WHERE candidate_id=?");
        upd.bind(1, cid); upd.exec();
        setMatchStatus("show", kairos_id, "matched", best);
    } else {
        std::cout << "[scraper]   \"" << title << "\" → uncertain"
                  << " (" << std::fixed << std::setprecision(2) << best << ")\n";
        setMatchStatus("show", kairos_id, "uncertain", best);
    }
}

void ScraperManager::matchMovie(const std::string& kairos_id, const std::string& title,
                                 int year, const std::string& tmdb_id,
                                 const std::string& file_path) {
    if (!tmdb_id.empty()) {
        if (!file_path.empty()) {
            std::string mapped = conf_.applyPathMap(file_path);
            if (!folderExists(mapped)) {
                std::cout << "[scraper]   \"" << title << "\" → uncertain (path missing)\n";
                storeCandidate("movie", kairos_id, "tmdb", tmdb_id, title, year, 1.0, "", "");
                setMatchStatus("movie", kairos_id, "uncertain", 1.0);
                return;
            }
            std::string folder = stripFolderYear(fs::path(mapped).parent_path().filename().string());
            double sim = titleSimilarity(title, folder);
            if (sim < kFolderTitleThreshold) {
                std::cout << "[scraper]   \"" << title << "\" → uncertain"
                          << " (folder: \"" << folder << "\", sim="
                          << std::fixed << std::setprecision(2) << sim << ")\n";
                storeCandidate("movie", kairos_id, "tmdb", tmdb_id, title, year, 1.0, "", "");
                setMatchStatus("movie", kairos_id, "uncertain", 1.0);
                return;
            }
        }
        std::cout << "[scraper]   \"" << title << "\" → matched\n";
        setMatchStatus("movie", kairos_id, "matched", 1.0);
        return;
    }

    if (title.empty()) {
        std::cout << "[scraper]   (no title, id=" << kairos_id << ") → unmatched\n";
        setMatchStatus("movie", kairos_id, "unmatched", 0.0);
        return;
    }

    struct Cand { std::string source, ext_id, cand_title, poster, overview; int cand_year; double score; };
    std::vector<Cand> candidates;

    auto collect = [&](const std::string& source, std::vector<Movie> results) {
        for (auto& r : results) {
            double sc = computeScore(title, year, r.title,
                                     r.year.has_value() ? r.year.value() : 0);
            std::string ext = (source == "tmdb") ? r.tmdb_id : r.imdb_id;
            if (ext.empty()) continue;
            candidates.push_back({ source, ext, r.title, r.thumb, r.overview,
                                   r.year.has_value() ? r.year.value() : 0, sc });
        }
    };

    if (tmdb_) collect("tmdb", tmdb_->searchMovies(title, year));
    if (tvdb_) collect("tvdb", tvdb_->searchMovies(title, year));

    double best = 0.0;
    for (const auto& c : candidates) {
        storeCandidate("movie", kairos_id, c.source, c.ext_id,
                        c.cand_title, c.cand_year, c.score, c.poster, c.overview);
        if (c.score > best) best = c.score;
    }

    if (candidates.empty()) {
        std::cout << "[scraper]   \"" << title << "\" → unmatched\n";
        setMatchStatus("movie", kairos_id, "unmatched", 0.0);
    } else if (best >= threshold()) {
        const auto& best_c = *std::max_element(candidates.begin(), candidates.end(),
            [](const Cand& a, const Cand& b){ return a.score < b.score; });
        std::cout << "[scraper]   \"" << title << "\" → matched"
                  << " (" << best_c.source << ", "
                  << std::fixed << std::setprecision(2) << best << ")\n";
        std::string cid = candidateKey("movie", kairos_id, best_c.source, best_c.ext_id);
        SQLite::Statement upd(db_.get(),
            "UPDATE item_match_candidate SET accepted=1 WHERE candidate_id=?");
        upd.bind(1, cid); upd.exec();
        setMatchStatus("movie", kairos_id, "matched", best);
    } else {
        std::cout << "[scraper]   \"" << title << "\" → uncertain"
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
    txn.commit();

    // Best-effort: fetch and apply full metadata from the scraper
    if (source == "tmdb" && tmdb_) {
        if (item_type == "show") {
            auto show = tmdb_->fetchShow(external_id);
            if (show) {
                SQLite::Statement app(db_.get(), R"(
                    UPDATE show SET
                        tmdb_id   = CASE WHEN locked THEN tmdb_id   ELSE ? END,
                        tvdb_id   = CASE WHEN locked THEN tvdb_id   ELSE ? END,
                        imdb_id   = CASE WHEN locked THEN imdb_id   ELSE ? END,
                        overview  = CASE WHEN locked THEN overview  ELSE ? END,
                        status    = CASE WHEN locked THEN status    ELSE ? END,
                        genres    = CASE WHEN locked THEN genres    ELSE ? END,
                        network   = CASE WHEN locked THEN network   ELSE ? END,
                        studio    = CASE WHEN locked THEN studio    ELSE ? END
                    WHERE show_id = ?
                )");
                app.bind(1, show->tmdb_id); app.bind(2, show->tvdb_id);
                app.bind(3, show->imdb_id); app.bind(4, show->overview);
                app.bind(5, show->status);  app.bind(6, show->genres);
                app.bind(7, show->network); app.bind(8, show->studio);
                app.bind(9, kairos_id);
                app.exec();
            }
        } else if (item_type == "movie") {
            auto movie = tmdb_->fetchMovie(external_id);
            if (movie) {
                SQLite::Statement app(db_.get(), R"(
                    UPDATE movie SET
                        tmdb_id  = CASE WHEN locked THEN tmdb_id  ELSE ? END,
                        imdb_id  = CASE WHEN locked THEN imdb_id  ELSE ? END,
                        overview = CASE WHEN locked THEN overview ELSE ? END,
                        genres   = CASE WHEN locked THEN genres   ELSE ? END,
                        studio   = CASE WHEN locked THEN studio   ELSE ? END,
                        director = CASE WHEN locked THEN director ELSE ? END
                    WHERE movie_id = ?
                )");
                app.bind(1, movie->tmdb_id); app.bind(2, movie->imdb_id);
                app.bind(3, movie->overview); app.bind(4, movie->genres);
                app.bind(5, movie->studio);  app.bind(6, movie->director);
                app.bind(7, kairos_id);
                app.exec();
            }
        }
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
            out.push_back(std::move(qi));
        }
    }

    // Movies
    sql = R"(
        SELECT m.movie_id, 'movie', m.title, COALESCE(m.year,0), m.thumb,
               sm.source_id, COALESCE(ms.base_url,''), m.match_status, COALESCE(m.match_score,0)
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

// ── Live search ───────────────────────────────────────────────────────────────

std::vector<ScraperManager::SearchResult>
ScraperManager::search(const std::string& query, const std::string& content_type) const {
    std::vector<SearchResult> out;

    auto addShow = [&](const std::string& source, const Show& s) {
        SearchResult r;
        r.source       = source;
        r.external_id  = (source == "tmdb") ? s.tmdb_id : s.tvdb_id;
        r.title        = s.title;
        r.year         = s.year.has_value() ? s.year.value() : 0;
        r.overview     = s.overview;
        r.poster_url   = s.thumb;
        r.content_type = "show";
        // Check if in library by tmdb_id or tvdb_id
        if (!s.tmdb_id.empty()) {
            SQLite::Statement chk(db_.get(),
                "SELECT 1 FROM show WHERE tmdb_id=? LIMIT 1");
            chk.bind(1, s.tmdb_id);
            r.in_library = chk.executeStep();
        } else if (!s.tvdb_id.empty()) {
            SQLite::Statement chk(db_.get(),
                "SELECT 1 FROM show WHERE tvdb_id=? LIMIT 1");
            chk.bind(1, s.tvdb_id);
            r.in_library = chk.executeStep();
        }
        if (!r.external_id.empty()) out.push_back(std::move(r));
    };

    auto addMovie = [&](const std::string& source, const Movie& m) {
        SearchResult r;
        r.source       = source;
        r.external_id  = m.tmdb_id;
        r.title        = m.title;
        r.year         = m.year.has_value() ? m.year.value() : 0;
        r.overview     = m.overview;
        r.poster_url   = m.thumb;
        r.content_type = "movie";
        if (!m.tmdb_id.empty()) {
            SQLite::Statement chk(db_.get(),
                "SELECT 1 FROM movie WHERE tmdb_id=? LIMIT 1");
            chk.bind(1, m.tmdb_id);
            r.in_library = chk.executeStep();
        }
        if (!r.external_id.empty()) out.push_back(std::move(r));
    };

    // Direct ID lookup: "tmdb:NNN" or "tvdb:NNN"
    std::string id_source, id_value;
    if (query.starts_with("tmdb:") && query.size() > 5) {
        id_source = "tmdb"; id_value = query.substr(5);
    } else if (query.starts_with("tvdb:") && query.size() > 5) {
        id_source = "tvdb"; id_value = query.substr(5);
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
        return out;
    }

    // Text search
    if (content_type == "show" || content_type.empty()) {
        if (tmdb_) for (auto& s : tmdb_->searchShows(query)) addShow("tmdb", s);
        if (tvdb_) for (auto& s : tvdb_->searchShows(query)) addShow("tvdb", s);
    }
    if (content_type == "movie" || content_type.empty()) {
        if (tmdb_) for (auto& m : tmdb_->searchMovies(query)) addMovie("tmdb", m);
        if (tvdb_) for (auto& m : tvdb_->searchMovies(query)) addMovie("tvdb", m);
    }
    return out;
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
    for (const auto* tbl : { "show", "movie" }) {
        s.total     += count(tbl, "matched")   + count(tbl, "uncertain")
                      + count(tbl, "unmatched") + count(tbl, "unscraped");
        s.matched   += count(tbl, "matched");
        s.uncertain += count(tbl, "uncertain");
        s.unmatched += count(tbl, "unmatched");
        s.unscraped += count(tbl, "unscraped");
    }
    return s;
}
