#include "RuleEngine.h"
#include "../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <numeric>
#include <unordered_set>

using json = nlohmann::json;

// ── Portability helpers ───────────────────────────────────────────────────────

static std::tm toUTC(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

// Decompose a UTC epoch into calendar components in the given IANA timezone.
// Falls back to UTC on unknown zone names.
static std::tm toChannelTZ(std::time_t t, const std::string& tz_name) {
    try {
        using namespace std::chrono;
        auto tp   = system_clock::from_time_t(t);
        const time_zone* zone = locate_zone(tz_name.empty() ? "UTC" : tz_name);
        zoned_time<system_clock::duration> zt{zone, tp};
        auto lt = zt.get_local_time();
        auto dp = floor<days>(lt);
        // Convert local_days → sys_days for year_month_day (same epoch, MSVC-safe).
        year_month_day ymd{sys_days{dp.time_since_epoch()}};

        // Time-of-day: avoid hms<> — MSVC mis-parses it as a function template.
        int total_sec = static_cast<int>(duration_cast<seconds>(lt - dp).count());
        int hour = total_sec / 3600;
        int min  = (total_sec % 3600) / 60;
        int sec  = total_sec % 60;

        // Day of week: 1970-01-01 was a Thursday (0=Sun, so Thursday=4).
        // Shift epoch days by +4 and wrap to [0,6].
        int epoch_day = static_cast<int>(dp.time_since_epoch().count());
        int wday = ((epoch_day % 7) + 4 + 7) % 7;

        std::tm tm{};
        tm.tm_year = int(ymd.year()) - 1900;
        tm.tm_mon  = unsigned(ymd.month()) - 1;
        tm.tm_mday = unsigned(ymd.day());
        tm.tm_hour = hour;
        tm.tm_min  = min;
        tm.tm_sec  = sec;
        tm.tm_wday = wday;
        return tm;
    } catch (...) {
        return toUTC(t);
    }
}

// ── Parsing helpers ───────────────────────────────────────────────────────────

static int parseTimeMins(const std::string& s) {
    // "HH:MM" → minutes from midnight
    return std::stoi(s.substr(0, 2)) * 60 + std::stoi(s.substr(3, 2));
}

static int dayBit(const std::tm& tm) {
    // Sun=1 Mon=2 Tue=4 … Sat=64 (matches day_mask convention)
    return 1 << tm.tm_wday;
}

static BlockType parseBlockType(const std::string& s) {
    if (s == "premier") return BlockType::Premier;
    if (s == "filler")  return BlockType::Filler;
    if (s == "movie")   return BlockType::Movie;
    return BlockType::Episode;
}

static Advancement parseAdvancement(const std::string& s) {
    if (s == "shuffle")       return Advancement::Shuffle;
    if (s == "smart_shuffle") return Advancement::SmartShuffle;
    if (s == "rerun_shuffle") return Advancement::RerunShuffle;
    if (s == "rerun_smart")   return Advancement::RerunSmart;
    return Advancement::Sequential;
}

static bool isRerunMode(Advancement a) {
    return a == Advancement::RerunShuffle || a == Advancement::RerunSmart;
}

static CursorScope parseCursorScope(const std::string& s) {
    if (s == "global")  return CursorScope::Global;
    if (s == "channel") return CursorScope::Channel;
    return CursorScope::Block;
}

static NoHistoryBehavior parseNoHistoryBehavior(const std::string& s) {
    if (s == "fallback_all") return NoHistoryBehavior::FallbackAll;
    if (s == "exclude")      return NoHistoryBehavior::Exclude;
    if (s == "filler")       return NoHistoryBehavior::Filler;
    if (s == "skip")         return NoHistoryBehavior::Skip;
    return NoHistoryBehavior::Normal;
}

// ── SimState JSON serialization ───────────────────────────────────────────────

static json simStateToJson(const RuleEngine::SimState& s) {
    json j;
    j["show_pos"]   = json::object();
    j["block_rr"]   = json::object();
    j["rerun_sel"]  = json::object();
    j["rerun_runs"] = json::object();
    for (const auto& [k, v] : s.show_pos)   j["show_pos"][k]   = v;
    for (const auto& [k, v] : s.block_rr)   j["block_rr"][k]   = v;
    for (const auto& [k, v] : s.rerun_sel)  j["rerun_sel"][k]  = v;
    for (const auto& [k, v] : s.rerun_runs) j["rerun_runs"][k] = v;
    return j;
}

static RuleEngine::SimState simStateFromJson(const std::string& raw) {
    RuleEngine::SimState s;
    if (raw.empty() || raw == "{}") return s;
    try {
        auto j = json::parse(raw);
        if (j.contains("show_pos"))
            for (auto& [k, v] : j["show_pos"].items())   s.show_pos[k]   = v.get<int>();
        if (j.contains("block_rr"))
            for (auto& [k, v] : j["block_rr"].items())   s.block_rr[k]   = v.get<int>();
        if (j.contains("rerun_sel"))
            for (auto& [k, v] : j["rerun_sel"].items())  s.rerun_sel[k]  = v.get<int>();
        if (j.contains("rerun_runs"))
            for (auto& [k, v] : j["rerun_runs"].items()) s.rerun_runs[k] = v.get<int>();
    } catch (...) {}
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────

RuleEngine::RuleEngine(Database& db) : db_(db) {}

// ── Static helpers ────────────────────────────────────────────────────────────

std::string RuleEngine::scopeStr(const Block& b) {
    switch (b.cursor_scope) {
        case CursorScope::Global:  return "global";
        case CursorScope::Channel: return "channel";
        case CursorScope::Block:   return "block";
    }
    return "block";
}

std::string RuleEngine::scopeId(const Block& b, const std::string& channel_id) {
    switch (b.cursor_scope) {
        case CursorScope::Global:  return "";
        case CursorScope::Channel: return channel_id;
        case CursorScope::Block:   return b.block_id;
    }
    return b.block_id;
}

// ── DB loading ────────────────────────────────────────────────────────────────

std::vector<Block> RuleEngine::loadBlocks(const std::string& channel_id) {
    std::vector<Block> blocks;
    SQLite::Statement q(db_.get(), R"(
        SELECT block_id, block_type, day_mask, start_time, end_time,
               program_count, priority, max_content_rating, advancement, cursor_scope,
               late_start_mins, align_to_mins, inter_filler, early_start_secs,
               filler_selection, smart_pct, start_scope, no_history_behavior
        FROM block WHERE channel_id = ?
        ORDER BY priority DESC
    )");
    q.bind(1, channel_id);

    while (q.executeStep()) {
        Block b;
        b.block_id           = q.getColumn(0).getString();
        b.channel_id         = channel_id;
        b.block_type         = parseBlockType(q.getColumn(1).getString());
        b.day_mask           = q.getColumn(2).getInt();
        b.start_time         = q.getColumn(3).getString();
        if (!q.getColumn(4).isNull())
            b.end_time = q.getColumn(4).getString();
        b.program_count      = q.getColumn(5).getInt();
        b.priority           = q.getColumn(6).getInt();
        b.max_content_rating = q.getColumn(7).getString();
        b.advancement        = parseAdvancement(q.getColumn(8).getString());
        b.cursor_scope       = parseCursorScope(q.getColumn(9).getString());
        b.late_start_mins    = q.getColumn(10).getInt();
        b.align_to_mins      = q.getColumn(11).getInt();
        b.inter_filler       = q.getColumn(12).getInt() != 0;
        b.early_start_secs   = q.getColumn(13).getInt();
        b.filler_selection   = q.getColumn(14).getString();
        b.smart_pct           = q.getColumn(15).getInt();
        b.start_scope         = q.getColumn(16).getString();
        b.no_history_behavior = parseNoHistoryBehavior(q.getColumn(17).getString());

        SQLite::Statement cq(db_.get(), R"(
            SELECT id, content_type, content_id, position, season_filter, weight, run_count
            FROM block_content WHERE block_id = ? ORDER BY position
        )");
        cq.bind(1, b.block_id);
        while (cq.executeStep()) {
            BlockContent bc;
            bc.id           = cq.getColumn(0).getInt();
            bc.block_id     = b.block_id;
            bc.content_type = cq.getColumn(1).getString();
            bc.content_id   = cq.getColumn(2).getString();
            bc.position     = cq.getColumn(3).getInt();
            if (!cq.getColumn(4).isNull())
                bc.season_filter = cq.getColumn(4).getInt();
            bc.weight     = cq.getColumn(5).getInt();
            bc.run_count  = cq.getColumn(6).getInt();
            b.content.push_back(std::move(bc));
        }

        SQLite::Statement fq(db_.get(), R"(
            SELECT filler_list_id, advancement, weight
            FROM block_filler_entry WHERE block_id = ? ORDER BY position
        )");
        fq.bind(1, b.block_id);
        while (fq.executeStep()) {
            BlockFillerEntry fe;
            fe.filler_list_id = fq.getColumn(0).getString();
            fe.advancement    = fq.getColumn(1).getString();
            fe.weight         = fq.getColumn(2).getInt();
            b.filler_entries.push_back(std::move(fe));
        }

        blocks.push_back(std::move(b));
    }
    return blocks;
}

std::vector<Episode> RuleEngine::getEpisodes(const std::string& show_id,
                                              std::optional<int> season) {
    std::vector<Episode> eps;
    const char* sql_filtered = R"(
        SELECT episode_id, show_id, season, episode, title, file_path, duration_ms,
               overview, air_date, thumb
        FROM episode WHERE show_id = ? AND season = ? ORDER BY season, episode
    )";
    const char* sql_all = R"(
        SELECT episode_id, show_id, season, episode, title, file_path, duration_ms,
               overview, air_date, thumb
        FROM episode WHERE show_id = ? ORDER BY season, episode
    )";

    SQLite::Statement q(db_.get(), season ? sql_filtered : sql_all);
    q.bind(1, show_id);
    if (season) q.bind(2, *season);

    while (q.executeStep()) {
        Episode e;
        e.episode_id  = q.getColumn(0).getString();
        e.show_id     = q.getColumn(1).getString();
        e.season      = q.getColumn(2).getInt();
        e.episode     = q.getColumn(3).getInt();
        e.title       = q.getColumn(4).getString();
        e.file_path   = q.getColumn(5).getString();
        e.duration_ms = q.getColumn(6).getInt64();
        e.overview    = q.getColumn(7).getString();
        e.air_date    = q.getColumn(8).getString();
        e.thumb       = q.getColumn(9).getString();
        eps.push_back(std::move(e));
    }
    return eps;
}

std::vector<Episode> RuleEngine::getPlayedEpisodes(const std::string& show_id,
                                                     const std::string& channel_id,
                                                     std::optional<int> season) {
    std::vector<Episode> eps;
    const char* sql_filtered = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb
        FROM episode e
        WHERE e.show_id = ? AND e.season = ?
          AND EXISTS (
              SELECT 1 FROM play_history ph
              WHERE ph.item_type = 'episode' AND ph.item_id = e.episode_id
                AND ph.channel_id = ?
          )
        ORDER BY e.season, e.episode
    )";
    const char* sql_all = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb
        FROM episode e
        WHERE e.show_id = ?
          AND EXISTS (
              SELECT 1 FROM play_history ph
              WHERE ph.item_type = 'episode' AND ph.item_id = e.episode_id
                AND ph.channel_id = ?
          )
        ORDER BY e.season, e.episode
    )";

    SQLite::Statement q(db_.get(), season ? sql_filtered : sql_all);
    q.bind(1, show_id);
    if (season) { q.bind(2, *season); q.bind(3, channel_id); }
    else         { q.bind(2, channel_id); }

    while (q.executeStep()) {
        Episode e;
        e.episode_id  = q.getColumn(0).getString();
        e.show_id     = q.getColumn(1).getString();
        e.season      = q.getColumn(2).getInt();
        e.episode     = q.getColumn(3).getInt();
        e.title       = q.getColumn(4).getString();
        e.file_path   = q.getColumn(5).getString();
        e.duration_ms = q.getColumn(6).getInt64();
        e.overview    = q.getColumn(7).getString();
        e.air_date    = q.getColumn(8).getString();
        e.thumb       = q.getColumn(9).getString();
        eps.push_back(std::move(e));
    }
    return eps;
}

std::vector<Episode> RuleEngine::getPlayedEpisodesWithCooldown(const std::string& show_id,
                                                                  const std::string& channel_id,
                                                                  std::optional<int> season,
                                                                  int smart_pct) {
    // Full played pool ordered oldest→newest.
    const char* sql_filtered = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.channel_id=?
        WHERE e.show_id=? AND e.season=?
        GROUP BY e.episode_id
        ORDER BY last_aired ASC
    )";
    const char* sql_all = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.channel_id=?
        WHERE e.show_id=?
        GROUP BY e.episode_id
        ORDER BY last_aired ASC
    )";

    SQLite::Statement q(db_.get(), season ? sql_filtered : sql_all);
    q.bind(1, channel_id); q.bind(2, show_id);
    if (season) q.bind(3, *season);

    std::vector<Episode> all;
    while (q.executeStep()) {
        Episode e;
        e.episode_id  = q.getColumn(0).getString();
        e.show_id     = q.getColumn(1).getString();
        e.season      = q.getColumn(2).getInt();
        e.episode     = q.getColumn(3).getInt();
        e.title       = q.getColumn(4).getString();
        e.file_path   = q.getColumn(5).getString();
        e.duration_ms = q.getColumn(6).getInt64();
        e.overview    = q.getColumn(7).getString();
        e.air_date    = q.getColumn(8).getString();
        e.thumb       = q.getColumn(9).getString();
        all.push_back(std::move(e));
    }
    if (all.empty()) return all;

    // Exclude the most-recently-played smart_pct% from the eligible pool.
    int cooldown = std::max(0, static_cast<int>(all.size()) * smart_pct / 100);
    // all is sorted oldest→newest; the last `cooldown` entries are the hot ones.
    int eligible_count = static_cast<int>(all.size()) - cooldown;
    if (eligible_count <= 0) return all; // every episode is hot — fall back to full pool
    all.resize(static_cast<size_t>(eligible_count));
    return all;
}

// ── Shuffle helpers ───────────────────────────────────────────────────────────

std::vector<int> RuleEngine::shufflePermutation(const std::string& seed_str, int n) {
    std::vector<int> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::mt19937_64 rng(std::hash<std::string>{}(seed_str));
    std::shuffle(order.begin(), order.end(), rng);
    return order;
}

int RuleEngine::selectWeighted(const Block& block, std::mt19937_64& rng) {
    int total = 0;
    for (const auto& bc : block.content) total += std::max(1, bc.weight);
    std::uniform_int_distribution<int> dist(0, total - 1);
    int r = dist(rng);
    for (int i = 0; i < static_cast<int>(block.content.size()); ++i) {
        r -= std::max(1, block.content[i].weight);
        if (r < 0) return i;
    }
    return static_cast<int>(block.content.size()) - 1;
}

int RuleEngine::snapToGroupStart(const std::string& episode_id,
                                  const std::vector<Episode>& eps) const {
    // Find if this episode belongs to a multipart group with part_num > 1.
    SQLite::Statement q(db_.get(), R"(
        SELECT egm2.episode_id
        FROM episode_group_member egm1
        JOIN episode_group_member egm2 ON egm2.group_id = egm1.group_id AND egm2.part_num = 1
        WHERE egm1.episode_id = ? AND egm1.part_num > 1
    )");
    q.bind(1, episode_id);
    if (!q.executeStep()) return -1; // not a mid-group episode
    std::string part1_id = q.getColumn(0).getString();
    for (int i = 0; i < static_cast<int>(eps.size()); ++i)
        if (eps[i].episode_id == part1_id) return i;
    return -1;
}

// ── Rerun state helpers ───────────────────────────────────────────────────────

int RuleEngine::readRunsRemaining(const std::string& block_id,
                                   const std::string& channel_id) {
    SQLite::Statement q(db_.get(),
        "SELECT runs_remaining FROM block_state WHERE block_id=? AND channel_id=?");
    q.bind(1, block_id); q.bind(2, channel_id);
    if (q.executeStep()) return q.getColumn(0).getInt();
    return 0;
}

void RuleEngine::writeRerunState(const std::string& block_id,
                                  const std::string& channel_id,
                                  int content_pos, int runs_remaining) {
    SQLite::Statement q(db_.get(), R"(
        INSERT INTO block_state (block_id, channel_id, content_position, runs_remaining, updated_at)
        VALUES (?,?,?,?,?)
        ON CONFLICT(block_id, channel_id)
        DO UPDATE SET content_position=excluded.content_position,
                      runs_remaining=excluded.runs_remaining,
                      updated_at=excluded.updated_at
    )");
    q.bind(1, block_id); q.bind(2, channel_id); q.bind(3, content_pos);
    q.bind(4, runs_remaining); q.bind(5, static_cast<int64_t>(std::time(nullptr)));
    q.exec();
}

std::optional<Movie> RuleEngine::getMovie(const std::string& movie_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT movie_id, title, content_rating, file_path, duration_ms, year,
               overview, tagline, studio, director, genres, thumb, art, imdb_id, tmdb_id
        FROM movie WHERE movie_id = ?
    )");
    q.bind(1, movie_id);
    if (!q.executeStep()) return std::nullopt;

    Movie m;
    m.movie_id       = q.getColumn(0).getString();
    m.title          = q.getColumn(1).getString();
    m.content_rating = q.getColumn(2).getString();
    m.file_path      = q.getColumn(3).getString();
    m.duration_ms    = q.getColumn(4).getInt64();
    if (!q.getColumn(5).isNull()) m.year = q.getColumn(5).getInt();
    m.overview  = q.getColumn(6).getString();
    m.tagline   = q.getColumn(7).getString();
    m.studio    = q.getColumn(8).getString();
    m.director  = q.getColumn(9).getString();
    m.genres    = q.getColumn(10).getString();
    m.thumb     = q.getColumn(11).getString();
    m.art       = q.getColumn(12).getString();
    m.imdb_id   = q.getColumn(13).getString();
    m.tmdb_id   = q.getColumn(14).getString();
    return m;
}

std::string RuleEngine::showTitle(const std::string& show_id) {
    try {
        SQLite::Statement q(db_.get(), "SELECT title FROM show WHERE show_id=?");
        q.bind(1, show_id);
        if (q.executeStep()) return q.getColumn(0).getString();
    } catch (...) {}
    return {};
}

// ── Cursor I/O ────────────────────────────────────────────────────────────────

int RuleEngine::readCursorPos(const std::string& content_type,
                               const std::string& content_id,
                               const std::string& scope,
                               const std::string& scope_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT position FROM media_cursor
        WHERE content_type=? AND content_id=? AND cursor_scope=? AND scope_id=?
    )");
    q.bind(1, content_type); q.bind(2, content_id);
    q.bind(3, scope);        q.bind(4, scope_id);
    if (q.executeStep() && !q.getColumn(0).isNull())
        return q.getColumn(0).getInt();
    return 0;
}

void RuleEngine::writeCursorPos(const std::string& content_type,
                                 const std::string& content_id,
                                 const std::string& scope,
                                 const std::string& scope_id,
                                 int pos, const std::string& episode_id) {
    SQLite::Statement q(db_.get(), R"(
        INSERT INTO media_cursor
            (content_type, content_id, cursor_scope, scope_id, episode_id, position, updated_at)
        VALUES (?,?,?,?,?,?,?)
        ON CONFLICT(content_type, content_id, cursor_scope, scope_id)
        DO UPDATE SET position=excluded.position,
                      episode_id=excluded.episode_id,
                      updated_at=excluded.updated_at
    )");
    q.bind(1, content_type); q.bind(2, content_id);
    q.bind(3, scope);        q.bind(4, scope_id);
    if (episode_id.empty()) q.bind(5); else q.bind(5, episode_id);
    q.bind(6, pos);
    q.bind(7, static_cast<int64_t>(std::time(nullptr)));
    q.exec();
}

int RuleEngine::readBlockRR(const std::string& block_id, const std::string& channel_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT content_position FROM block_state WHERE block_id=? AND channel_id=?
    )");
    q.bind(1, block_id); q.bind(2, channel_id);
    if (q.executeStep()) return q.getColumn(0).getInt();
    return 0;
}

void RuleEngine::writeBlockRR(const std::string& block_id,
                               const std::string& channel_id, int pos) {
    SQLite::Statement q(db_.get(), R"(
        INSERT INTO block_state (block_id, channel_id, content_position, updated_at)
        VALUES (?,?,?,?)
        ON CONFLICT(block_id, channel_id)
        DO UPDATE SET content_position=excluded.content_position,
                      updated_at=excluded.updated_at
    )");
    q.bind(1, block_id); q.bind(2, channel_id);
    q.bind(3, pos);
    q.bind(4, static_cast<int64_t>(std::time(nullptr)));
    q.exec();
}

// ── Block resolution ──────────────────────────────────────────────────────────

std::optional<Block> RuleEngine::resolveFromList(const std::vector<Block>& blocks,
                                                  std::time_t t,
                                                  const std::string& tz) {
    auto tm      = toChannelTZ(t, tz);
    int  day_bit = dayBit(tm);
    int  cur_sec = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;

    const Block* best = nullptr;
    for (const auto& b : blocks) {
        if (!(b.day_mask & day_bit)) continue;
        int start_sec = parseTimeMins(b.start_time) * 60;
        // Align the block's nominal start to the next grid boundary.
        int eff_start_sec = start_sec;
        if (b.align_to_mins > 0) {
            int step    = b.align_to_mins * 60;
            int aligned = ((start_sec + step - 1) / step) * step;
            if (aligned < 86400) eff_start_sec = aligned; // clamp — don't wrap past midnight
        }
        // early_start_secs expands the match window backward from the effective start.
        if (cur_sec < eff_start_sec - b.early_start_secs) continue;
        if (b.end_time.has_value()) {
            if (cur_sec >= parseTimeMins(*b.end_time) * 60) continue;
        }
        // blocks are pre-sorted priority DESC; first match wins
        if (!best) best = &b;
    }
    if (!best) return std::nullopt;
    return *best;
}

std::string RuleEngine::channelTimezone(const std::string& channel_id) {
    SQLite::Statement q(db_.get(), "SELECT timezone FROM channel WHERE channel_id=?");
    q.bind(1, channel_id);
    if (q.executeStep()) {
        auto tz = q.getColumn(0).getString();
        if (!tz.empty()) return tz;
    }
    return "UTC";
}

std::optional<Block> RuleEngine::resolveBlock(const std::string& channel_id, std::time_t t) {
    auto blocks = loadBlocks(channel_id);
    return resolveFromList(blocks, t, channelTimezone(channel_id));
}

// ── Item selection ────────────────────────────────────────────────────────────

// Shared logic for filling in a ScheduledItem from a show's episode list.
static std::optional<ScheduledItem> itemFromShow(
    const std::string& channel_id,
    const std::string& block_id,
    const std::vector<Episode>& eps,
    int ep_pos,
    const std::string& show_title_str) {

    if (eps.empty()) return std::nullopt;
    ep_pos = ep_pos % static_cast<int>(eps.size());
    const auto& ep = eps[ep_pos];

    ScheduledItem item;
    item.item_type   = "episode";
    item.item_id     = ep.episode_id;
    item.file_path   = ep.file_path;
    item.duration_ms = ep.duration_ms;
    item.title       = ep.title;
    item.show_title  = show_title_str;
    item.show_id     = ep.show_id;
    item.season      = ep.season;
    item.episode_num = ep.episode;
    item.channel_id  = channel_id;
    item.block_id    = block_id;
    return item;
}

std::optional<ScheduledItem> RuleEngine::nextItem(const std::string& channel_id,
                                                    const Block& block) {
    if (block.content.empty()) return std::nullopt;

    int  n  = static_cast<int>(block.content.size());
    int  rr = readBlockRR(block.block_id, channel_id) % n;
    const auto& bc = block.content[rr];

    if (bc.content_type == "show") {
        if (isRerunMode(block.advancement)) {
            // Rerun: content_position in block_state holds the selected show index.
            auto eps = (block.advancement == Advancement::RerunSmart)
                ? getPlayedEpisodesWithCooldown(bc.content_id, channel_id, bc.season_filter, block.smart_pct)
                : getPlayedEpisodes(bc.content_id, channel_id, bc.season_filter);
            if (eps.empty()) {
                switch (block.no_history_behavior) {
                    case NoHistoryBehavior::Normal: {
                        // Play as a regular show: all episodes, sequential show cursor.
                        auto all = getEpisodes(bc.content_id, bc.season_filter);
                        if (all.empty()) return std::nullopt;
                        int pos = readCursorPos("show", bc.content_id,
                                                scopeStr(block), scopeId(block, channel_id));
                        return itemFromShow(channel_id, block.block_id, all, pos, showTitle(bc.content_id));
                    }
                    case NoHistoryBehavior::FallbackAll:
                        // Treat full catalog as the rerun pool; fall through with all eps.
                        eps = getEpisodes(bc.content_id, bc.season_filter);
                        if (eps.empty()) return std::nullopt;
                        break;
                    default:
                        // Exclude, Filler, Skip: no content for this slot.
                        return std::nullopt;
                }
            }
            int pos = readCursorPos("show_rerun", bc.content_id, "block", block.block_id);
            return itemFromShow(channel_id, block.block_id, eps, pos, showTitle(bc.content_id));
        }
        auto eps = getEpisodes(bc.content_id, bc.season_filter);
        if (eps.empty()) return std::nullopt;
        int pos = readCursorPos("show", bc.content_id, scopeStr(block), scopeId(block, channel_id));
        if (block.advancement == Advancement::Shuffle || block.advancement == Advancement::SmartShuffle) {
            int epoch = pos / static_cast<int>(eps.size());
            int idx   = pos % static_cast<int>(eps.size());
            auto perm = shufflePermutation(bc.content_id + std::to_string(epoch), static_cast<int>(eps.size()));
            return itemFromShow(channel_id, block.block_id, eps, perm[idx], showTitle(bc.content_id));
        }
        return itemFromShow(channel_id, block.block_id, eps, pos, showTitle(bc.content_id));
    }
    if (bc.content_type == "movie") {
        auto m = getMovie(bc.content_id);
        if (!m) return std::nullopt;
        ScheduledItem item;
        item.item_type   = "movie";
        item.item_id     = m->movie_id;
        item.file_path   = m->file_path;
        item.duration_ms = m->duration_ms;
        item.title       = m->title;
        item.channel_id  = channel_id;
        item.block_id    = block.block_id;
        return item;
    }
    if (bc.content_type == "episode") {
        SQLite::Statement q(db_.get(), R"(
            SELECT e.episode_id, e.show_id, e.season, e.episode,
                   e.title, e.file_path, e.duration_ms, s.title
            FROM episode e LEFT JOIN show s ON s.show_id = e.show_id
            WHERE e.episode_id=?
        )");
        q.bind(1, bc.content_id);
        if (!q.executeStep()) return std::nullopt;
        ScheduledItem item;
        item.item_type   = "episode";
        item.item_id     = q.getColumn(0).getString();
        item.show_id     = q.getColumn(1).getString();
        item.season      = q.getColumn(2).getInt();
        item.episode_num = q.getColumn(3).getInt();
        item.title       = q.getColumn(4).getString();
        item.file_path   = q.getColumn(5).getString();
        item.duration_ms = q.getColumn(6).getInt64();
        item.show_title  = q.getColumn(7).getString();
        item.channel_id  = channel_id;
        item.block_id    = block.block_id;
        return item;
    }
    if (bc.content_type == "playlist" || bc.content_type == "filler_list") {
        bool is_fl = (bc.content_type == "filler_list");
        std::vector<std::pair<std::string, std::string>> items;
        {
            const char* sql = is_fl
                ? "SELECT item_type, item_id FROM filler_list_item WHERE filler_list_id=? ORDER BY position"
                : "SELECT item_type, item_id FROM playlist_item     WHERE playlist_id=?    ORDER BY position";
            SQLite::Statement q(db_.get(), sql);
            q.bind(1, bc.content_id);
            while (q.executeStep())
                items.push_back({ q.getColumn(0).getString(), q.getColumn(1).getString() });
        }
        if (items.empty()) return std::nullopt;

        int pos = readCursorPos(bc.content_type, bc.content_id, scopeStr(block), scopeId(block, channel_id));
        pos = pos % static_cast<int>(items.size());
        const auto& [ptype, pid] = items[pos];

        if (ptype == "episode") {
            SQLite::Statement q(db_.get(), R"(
                SELECT e.episode_id, e.show_id, e.season, e.episode,
                       e.title, e.file_path, e.duration_ms, s.title
                FROM episode e LEFT JOIN show s ON s.show_id = e.show_id
                WHERE e.episode_id=?
            )");
            q.bind(1, pid);
            if (!q.executeStep()) return std::nullopt;
            ScheduledItem item;
            item.item_type   = "episode";
            item.item_id     = q.getColumn(0).getString();
            item.show_id     = q.getColumn(1).getString();
            item.season      = q.getColumn(2).getInt();
            item.episode_num = q.getColumn(3).getInt();
            item.title       = q.getColumn(4).getString();
            item.file_path   = q.getColumn(5).getString();
            item.duration_ms = q.getColumn(6).getInt64();
            item.show_title  = q.getColumn(7).getString();
            item.channel_id  = channel_id;
            item.block_id    = block.block_id;
            return item;
        } else {
            auto m = getMovie(pid);
            if (!m) return std::nullopt;
            ScheduledItem item;
            item.item_type   = "movie";
            item.item_id     = m->movie_id;
            item.file_path   = m->file_path;
            item.duration_ms = m->duration_ms;
            item.title       = m->title;
            item.channel_id  = channel_id;
            item.block_id    = block.block_id;
            return item;
        }
    }
    return std::nullopt;
}

// ── Simulation (EPG read-only projection) ────────────────────────────────────

std::optional<ScheduledItem> RuleEngine::nextItemSim(const std::string& channel_id,
                                                      const Block& block,
                                                      SimState& state,
                                                      int seed) {
    if (block.content.empty()) return std::nullopt;

    int  n  = static_cast<int>(block.content.size());
    auto& rr = state.block_rr[block.block_id];
    rr = rr % n;

    const auto& bc = block.content[rr];

    if (bc.content_type == "show") {
        if (isRerunMode(block.advancement)) {
            // ── Rerun sim ───────────────────────────────────────────────────
            // rerun_sel[block_id] = selected content index
            // rerun_runs[block_id] = runs remaining for selected show
            auto& sel  = state.rerun_sel[block.block_id];
            auto& runs = state.rerun_runs[block.block_id];

            bool need_select = (runs <= 0);
            if (need_select) {
                std::mt19937_64 rng(static_cast<uint64_t>(seed >= 0 ? seed : 0)
                                    ^ std::hash<std::string>{}(block.block_id));
                if (block.no_history_behavior == NoHistoryBehavior::Exclude) {
                    // Weighted selection among only content entries with play history.
                    std::vector<int> eligible;
                    int total_w = 0;
                    for (int i = 0; i < n; i++) {
                        const auto& cbc = block.content[i];
                        if (cbc.content_type == "show") {
                            auto ceps = (block.advancement == Advancement::RerunSmart)
                                ? getPlayedEpisodesWithCooldown(cbc.content_id, channel_id, cbc.season_filter, block.smart_pct)
                                : getPlayedEpisodes(cbc.content_id, channel_id, cbc.season_filter);
                            if (ceps.empty()) continue;
                        }
                        eligible.push_back(i);
                        total_w += std::max(1, cbc.weight);
                    }
                    if (eligible.empty()) { runs = 0; return std::nullopt; }
                    std::uniform_int_distribution<int> dist(0, total_w - 1);
                    int r = dist(rng);
                    sel = eligible.back();
                    for (int idx : eligible) {
                        r -= std::max(1, block.content[idx].weight);
                        if (r < 0) { sel = idx; break; }
                    }
                } else {
                    sel = selectWeighted(block, rng);
                }
                runs = std::max(1, block.content[sel].run_count);
            }

            const auto& sel_bc = block.content[sel];
            auto eps = (block.advancement == Advancement::RerunSmart)
                ? getPlayedEpisodesWithCooldown(sel_bc.content_id, channel_id, sel_bc.season_filter, block.smart_pct)
                : getPlayedEpisodes(sel_bc.content_id, channel_id, sel_bc.season_filter);

            if (eps.empty()) {
                switch (block.no_history_behavior) {
                    case NoHistoryBehavior::Normal: {
                        // Play as a regular show: all episodes, sequential show cursor.
                        auto all = getEpisodes(sel_bc.content_id, sel_bc.season_filter);
                        if (all.empty()) { runs = 0; return std::nullopt; }
                        std::string norm_key = "rerun_normal:" + block.block_id + ":" + sel_bc.content_id;
                        if (!state.show_pos.count(norm_key))
                            state.show_pos[norm_key] = seed >= 0
                                ? (seed % static_cast<int>(all.size()))
                                : readCursorPos("show", sel_bc.content_id,
                                                scopeStr(block), scopeId(block, channel_id));
                        auto& ep_pos = state.show_pos[norm_key];
                        auto item = itemFromShow(channel_id, block.block_id, all, ep_pos,
                                                 showTitle(sel_bc.content_id));
                        ep_pos = (ep_pos + 1) % static_cast<int>(all.size());
                        --runs;
                        return item;
                    }
                    case NoHistoryBehavior::FallbackAll:
                        // Treat full catalog as the rerun pool; continue with rerun logic.
                        eps = getEpisodes(sel_bc.content_id, sel_bc.season_filter);
                        if (eps.empty()) { runs = 0; rr = (rr + 1) % n; return std::nullopt; }
                        break;
                    default:
                        // Exclude (shouldn't reach here after pre-filter), Filler, Skip.
                        runs = 0;
                        rr   = (rr + 1) % n;
                        return std::nullopt;
                }
            }

            std::string key = "rerun:" + block.block_id + ":" + sel_bc.content_id;
            if (!state.show_pos.count(key)) {
                // Random starting episode, snapped to multipart Part 1.
                std::mt19937_64 rng2(static_cast<uint64_t>(seed >= 0 ? seed : 42)
                                     ^ std::hash<std::string>{}(sel_bc.content_id));
                std::uniform_int_distribution<int> dist(0, static_cast<int>(eps.size()) - 1);
                int start = dist(rng2);
                int snap  = snapToGroupStart(eps[start].episode_id, eps);
                state.show_pos[key] = (snap >= 0) ? snap : start;
            }

            auto& ep_pos = state.show_pos[key];
            auto item = itemFromShow(channel_id, block.block_id, eps, ep_pos, showTitle(sel_bc.content_id));
            ep_pos = (ep_pos + 1) % static_cast<int>(eps.size());
            --runs;
            // Don't advance rr for rerun — it's managed by rerun_sel.
            return item;
        }

        // ── Non-rerun (sequential / shuffle / smart_shuffle) ────────────────
        auto eps = getEpisodes(bc.content_id, bc.season_filter);
        if (eps.empty()) { rr = (rr + 1) % n; return std::nullopt; }

        std::string key = scopeStr(block) + ":" + scopeId(block, channel_id) + ":" + bc.content_id;
        if (!state.show_pos.count(key)) {
            // Premier blocks always start from the live cursor — seed offsetting is
            // for episode/rerun blocks that can legitimately start mid-series.
            bool use_seed = (seed >= 0) && (block.block_type != BlockType::Premier);
            state.show_pos[key] = use_seed
                ? (seed % static_cast<int>(eps.size()))
                : readCursorPos("show", bc.content_id, scopeStr(block), scopeId(block, channel_id));
        }
        auto& ep_pos = state.show_pos[key];

        std::optional<ScheduledItem> item;
        if (block.advancement == Advancement::Shuffle || block.advancement == Advancement::SmartShuffle) {
            int epoch = ep_pos / static_cast<int>(eps.size());
            int idx   = ep_pos % static_cast<int>(eps.size());
            auto perm = shufflePermutation(bc.content_id + std::to_string(epoch), static_cast<int>(eps.size()));
            item = itemFromShow(channel_id, block.block_id, eps, perm[idx], showTitle(bc.content_id));
            ep_pos++; // grows monotonically; epoch derived implicitly
        } else {
            item = itemFromShow(channel_id, block.block_id, eps, ep_pos, showTitle(bc.content_id));
            ep_pos = (ep_pos + 1) % static_cast<int>(eps.size());
        }
        rr = (rr + 1) % n;
        return item;
    }
    if (bc.content_type == "movie") {
        auto m = getMovie(bc.content_id);
        rr = (rr + 1) % n;
        if (!m) return std::nullopt;
        ScheduledItem item;
        item.item_type   = "movie";
        item.item_id     = m->movie_id;
        item.file_path   = m->file_path;
        item.duration_ms = m->duration_ms;
        item.title       = m->title;
        item.channel_id  = channel_id;
        item.block_id    = block.block_id;
        return item;
    }
    if (bc.content_type == "episode") {
        rr = (rr + 1) % n;
        SQLite::Statement q(db_.get(), R"(
            SELECT e.episode_id, e.show_id, e.season, e.episode,
                   e.title, e.file_path, e.duration_ms, s.title
            FROM episode e LEFT JOIN show s ON s.show_id = e.show_id
            WHERE e.episode_id=?
        )");
        q.bind(1, bc.content_id);
        if (!q.executeStep()) return std::nullopt;
        ScheduledItem item;
        item.item_type   = "episode";
        item.item_id     = q.getColumn(0).getString();
        item.show_id     = q.getColumn(1).getString();
        item.season      = q.getColumn(2).getInt();
        item.episode_num = q.getColumn(3).getInt();
        item.title       = q.getColumn(4).getString();
        item.file_path   = q.getColumn(5).getString();
        item.duration_ms = q.getColumn(6).getInt64();
        item.show_title  = q.getColumn(7).getString();
        item.channel_id  = channel_id;
        item.block_id    = block.block_id;
        return item;
    }
    if (bc.content_type == "playlist" || bc.content_type == "filler_list") {
        rr = (rr + 1) % n;
        bool is_fl = (bc.content_type == "filler_list");
        std::vector<std::pair<std::string, std::string>> items;
        {
            const char* sql = is_fl
                ? "SELECT item_type, item_id FROM filler_list_item WHERE filler_list_id=? ORDER BY position"
                : "SELECT item_type, item_id FROM playlist_item     WHERE playlist_id=?    ORDER BY position";
            SQLite::Statement q(db_.get(), sql);
            q.bind(1, bc.content_id);
            while (q.executeStep())
                items.push_back({ q.getColumn(0).getString(), q.getColumn(1).getString() });
        }
        if (items.empty()) return std::nullopt;

        std::string key = bc.content_type + ":" + scopeStr(block) + ":" + scopeId(block, channel_id) + ":" + bc.content_id;
        if (!state.show_pos.count(key))
            state.show_pos[key] = (seed >= 0)
                ? (seed % static_cast<int>(items.size()))
                : readCursorPos(bc.content_type, bc.content_id, scopeStr(block), scopeId(block, channel_id));
        auto& pos = state.show_pos[key];
    pos = pos % static_cast<int>(items.size());
        const auto& [ptype, pid] = items[pos];
        pos = (pos + 1) % static_cast<int>(items.size());

        if (ptype == "episode") {
            SQLite::Statement q(db_.get(), R"(
                SELECT e.episode_id, e.show_id, e.season, e.episode,
                       e.title, e.file_path, e.duration_ms, s.title
                FROM episode e LEFT JOIN show s ON s.show_id = e.show_id
                WHERE e.episode_id=?
            )");
            q.bind(1, pid);
            if (!q.executeStep()) return std::nullopt;
            ScheduledItem item;
            item.item_type   = "episode";
            item.item_id     = q.getColumn(0).getString();
            item.show_id     = q.getColumn(1).getString();
            item.season      = q.getColumn(2).getInt();
            item.episode_num = q.getColumn(3).getInt();
            item.title       = q.getColumn(4).getString();
            item.file_path   = q.getColumn(5).getString();
            item.duration_ms = q.getColumn(6).getInt64();
            item.show_title  = q.getColumn(7).getString();
            item.channel_id  = channel_id;
            item.block_id    = block.block_id;
            return item;
        } else {
            auto m = getMovie(pid);
            if (!m) return std::nullopt;
            ScheduledItem item;
            item.item_type   = "movie";
            item.item_id     = m->movie_id;
            item.file_path   = m->file_path;
            item.duration_ms = m->duration_ms;
            item.title       = m->title;
            item.channel_id  = channel_id;
            item.block_id    = block.block_id;
            return item;
        }
    }
    rr = (rr + 1) % n;
    return std::nullopt;
}

// ── Inter-filler clip picker ──────────────────────────────────────────────────

std::optional<ScheduledItem> RuleEngine::pickFillerSim(
    const std::string& channel_id,
    const Block& block,
    const std::vector<BlockFillerEntry>& pool,
    int64_t max_ms,
    SimState& state,
    int seed)
{
    if (pool.empty()) return std::nullopt;
    int n = static_cast<int>(pool.size());

    // Select which filler entry (filler list) to pull from.
    int entry_idx = 0;
    if (block.filler_selection == "round_robin") {
        std::string rr_key = "fl_rr:" + block.block_id;
        int& rr   = state.show_pos[rr_key];
        entry_idx = rr % n;
        rr        = (rr + 1) % n;
    } else {
        std::mt19937_64 rng(static_cast<uint64_t>(seed >= 0 ? seed : 0)
                            ^ std::hash<std::string>{}(block.block_id)
                            ^ static_cast<uint64_t>(state.show_pos.size()));
        if (block.filler_selection == "weighted") {
            int total = 0;
            for (const auto& e : pool) total += std::max(1, e.weight);
            std::uniform_int_distribution<int> dist(0, total - 1);
            int r = dist(rng);
            for (int i = 0; i < n; ++i) {
                r -= std::max(1, pool[i].weight);
                if (r < 0) { entry_idx = i; break; }
            }
        } else { // random
            std::uniform_int_distribution<int> dist(0, n - 1);
            entry_idx = dist(rng);
        }
    }

    const auto& fe = pool[entry_idx];

    // Load items from the selected filler list (with durations for "sized" mode).
    struct FI { std::string type, id; int64_t dur = 0; };
    std::vector<FI> items;
    {
        SQLite::Statement q(db_.get(), R"(
            SELECT fi.item_type, fi.item_id,
                   COALESCE(e.duration_ms, m.duration_ms, 0)
            FROM filler_list_item fi
            LEFT JOIN episode e ON fi.item_type='episode' AND fi.item_id=e.episode_id
            LEFT JOIN movie   m ON fi.item_type='movie'   AND fi.item_id=m.movie_id
            WHERE fi.filler_list_id=? ORDER BY fi.position
        )");
        q.bind(1, fe.filler_list_id);
        while (q.executeStep())
            items.push_back({q.getColumn(0).getString(), q.getColumn(1).getString(),
                             q.getColumn(2).getInt64()});
    }
    if (items.empty()) return std::nullopt;

    // Determine item index within the list.
    int item_idx = 0;
    std::string pos_key = "fl_pos:" + fe.filler_list_id + ":" + block.block_id;

    if (fe.advancement == "sized") {
        // Pick the longest clip that fits within max_ms; bail if nothing fits.
        int best = -1; int64_t best_dur = -1;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if (items[i].dur <= max_ms && items[i].dur > best_dur)
                { best = i; best_dur = items[i].dur; }
        }
        if (best < 0) return std::nullopt;
        item_idx = best;
        // "sized" is stateless — best-fit pick each time, no cursor to advance.
    } else if (fe.advancement == "shuffle") {
        if (!state.show_pos.count(pos_key))
            state.show_pos[pos_key] = seed >= 0 ? seed % static_cast<int>(items.size()) : 0;
        int& pos   = state.show_pos[pos_key];
        int  epoch = pos / static_cast<int>(items.size());
        int  idx   = pos % static_cast<int>(items.size());
        auto perm  = shufflePermutation(
            "fl:" + fe.filler_list_id + ":" + block.block_id + std::to_string(epoch),
            static_cast<int>(items.size()));
        item_idx = perm[idx];
        ++pos;
    } else { // sequential
        if (!state.show_pos.count(pos_key))
            state.show_pos[pos_key] = seed >= 0 ? seed % static_cast<int>(items.size()) : 0;
        int& pos = state.show_pos[pos_key];
        item_idx = pos % static_cast<int>(items.size());
        pos      = (pos + 1) % static_cast<int>(items.size());
    }

    const auto& fi = items[item_idx];

    ScheduledItem item;
    item.is_filler = true;
    if (fi.type == "episode") {
        SQLite::Statement q(db_.get(), R"(
            SELECT e.episode_id, e.show_id, e.season, e.episode,
                   e.title, e.file_path, e.duration_ms, s.title
            FROM episode e LEFT JOIN show s ON s.show_id=e.show_id
            WHERE e.episode_id=?
        )");
        q.bind(1, fi.id);
        if (!q.executeStep()) return std::nullopt;
        item.item_type   = "episode";
        item.item_id     = q.getColumn(0).getString();
        item.show_id     = q.getColumn(1).getString();
        item.season      = q.getColumn(2).getInt();
        item.episode_num = q.getColumn(3).getInt();
        item.title       = q.getColumn(4).getString();
        item.file_path   = q.getColumn(5).getString();
        item.duration_ms = q.getColumn(6).getInt64();
        item.show_title  = q.getColumn(7).getString();
    } else {
        auto m = getMovie(fi.id);
        if (!m) return std::nullopt;
        item.item_type   = "movie";
        item.item_id     = m->movie_id;
        item.file_path   = m->file_path;
        item.duration_ms = m->duration_ms;
        item.title       = m->title;
    }
    item.channel_id = channel_id;
    item.block_id   = block.block_id;
    return item;
}

// ── Forward projection ────────────────────────────────────────────────────────

std::vector<ScheduledItem> RuleEngine::project(const std::string& channel_id,
                                                std::time_t start, int horizon_hours,
                                                const std::string& initial_cursor_json,
                                                int seed) {
    std::vector<ScheduledItem> result;
    auto blocks = loadBlocks(channel_id);
    if (blocks.empty()) return result;

    // Restore SimState from snapshot, or initialise from seed / DB.
    SimState state = simStateFromJson(initial_cursor_json);
    for (const auto& b : blocks) {
        if (isRerunMode(b.advancement)) {
            if (!state.rerun_runs.count(b.block_id)) {
                // Seed rerun state from DB; in seeded (preview) mode start fresh.
                state.rerun_sel[b.block_id]  = seed >= 0 ? 0 : readBlockRR(b.block_id, channel_id);
                state.rerun_runs[b.block_id] = seed >= 0 ? 0 : readRunsRemaining(b.block_id, channel_id);
            }
        } else {
            if (!state.block_rr.count(b.block_id)) {
                int n = static_cast<int>(b.content.size());
                state.block_rr[b.block_id] = seed >= 0 ? (n > 0 ? seed % n : 0)
                                                        : readBlockRR(b.block_id, channel_id);
            }
        }
    }

    // Channel-level filler entries: fallback when a block has no filler_entries.
    std::vector<BlockFillerEntry> channel_filler;
    {
        SQLite::Statement cfq(db_.get(), R"(
            SELECT filler_list_id, advancement, weight
            FROM channel_filler_entry WHERE channel_id = ? ORDER BY position
        )");
        cfq.bind(1, channel_id);
        while (cfq.executeStep()) {
            BlockFillerEntry fe;
            fe.filler_list_id = cfq.getColumn(0).getString();
            fe.advancement    = cfq.getColumn(1).getString();
            fe.weight         = cfq.getColumn(2).getInt();
            channel_filler.push_back(std::move(fe));
        }
    }

    const std::string tz = channelTimezone(channel_id);

    std::time_t t   = start;
    std::time_t end = start + static_cast<std::time_t>(horizon_hours) * 3600;
    const int MAX_ITEMS = std::max(2000, horizon_hours * 300); // ~12s clip floor per hour
    std::string prev_block_id;
    std::unordered_map<std::string, int> prog_counts;  // programs scheduled per block occurrence
    std::unordered_set<std::string>      exhausted_blocks; // blocks that hit program_count this day
    int prev_day = -1;

    while (t < end && static_cast<int>(result.size()) < MAX_ITEMS) {
        // Clear exhausted_blocks when the calendar day rolls over (channel-local time).
        {
            auto tm_chk  = toChannelTZ(t, tz);
            int  cur_day = tm_chk.tm_year * 1000 + tm_chk.tm_yday;
            if (cur_day != prev_day) { exhausted_blocks.clear(); prev_day = cur_day; }
        }

        auto block_opt = resolveFromList(blocks, t, tz);
        // If the top-priority block is exhausted for this occurrence, re-resolve
        // against the remaining blocks so a lower-priority active block takes over
        // instead of falling through to the no-block (future-start-only) handler.
        if (block_opt && exhausted_blocks.count(block_opt->block_id)) {
            std::vector<Block> active;
            active.reserve(blocks.size());
            for (const auto& b : blocks)
                if (!exhausted_blocks.count(b.block_id)) active.push_back(b);
            block_opt = resolveFromList(active, t, tz);
        }
        if (!block_opt) {
            // Jump to the nearest upcoming block start rather than a blind 30-min skip.
            // A 30-min hop can overshoot narrow boundaries (e.g. a block starting in 5
            // minutes), causing the first episode to air up to ~29 minutes late.
            auto   tm_t          = toChannelTZ(t, tz);
            int    cur_sec       = tm_t.tm_hour * 3600 + tm_t.tm_min * 60 + tm_t.tm_sec;
            std::time_t midnight = t - static_cast<std::time_t>(cur_sec);
            int today_bit        = dayBit(tm_t);
            int tom_bit          = 1 << ((tm_t.tm_wday + 1) % 7);

            std::time_t jump = t + 1800; // fallback if no closer block found
            for (const auto& b : blocks) {
                auto try_day = [&](int bit, std::time_t base_midnight) {
                    if (!(b.day_mask & bit)) return;
                    std::time_t cand = base_midnight
                        + static_cast<std::time_t>(parseTimeMins(b.start_time)) * 60;
                    if (cand > t && cand < jump) jump = cand;
                };
                try_day(today_bit, midnight);
                try_day(tom_bit,   midnight + 86400);
            }
            t = jump;
            prev_block_id.clear();
            continue;
        }
        const Block& block = *block_opt;

        // On first entry to a block (block-scope): compute effective start, enforce late_start_mins, apply alignment.
        if (block.block_id != prev_block_id && block.start_scope != "episode") {
            auto        tm_t    = toChannelTZ(t, tz);
            int         cur_sec = tm_t.tm_hour * 3600 + tm_t.tm_min * 60 + tm_t.tm_sec;
            std::time_t today_midnight = t - static_cast<std::time_t>(cur_sec);

            // Effective (aligned) start — aligned to channel-local time boundaries.
            std::time_t effective_start = t;
            if (block.align_to_mins > 0) {
                std::time_t step         = static_cast<std::time_t>(block.align_to_mins) * 60;
                std::time_t local_sec_up = ((static_cast<std::time_t>(cur_sec) + step - 1) / step) * step;
                effective_start          = today_midnight + local_sec_up;
            }

            // Nominal start for today (the block's raw start_time as a Unix timestamp).
            std::time_t nominal_start = today_midnight
                                      + static_cast<std::time_t>(parseTimeMins(block.start_time)) * 60;

            // late_start_mins: only enforce if the block's entire catch-up window falls
            // within this projection. If the late-window end (nominal_start + late_start_mins)
            // is at or before the projection start, the block was already in progress when
            // we began — including when it resumes after a higher-priority block exhausts.
            std::time_t late_window_end = nominal_start
                                        + static_cast<std::time_t>(block.late_start_mins) * 60;
            if (late_window_end > start && effective_start > late_window_end) {
                if (block.end_time.has_value()) {
                    std::time_t block_end = today_midnight
                                          + static_cast<std::time_t>(parseTimeMins(*block.end_time)) * 60;
                    t = (block_end > t) ? block_end + 1 : t + 1800;
                } else {
                    t += 1800;
                }
                // Do not commit prev_block_id — treat this block as not entered.
                continue;
            }

            prev_block_id = block.block_id;
            prog_counts[block.block_id] = 0;

            // Jump to effective start if alignment pushed us forward.
            if (effective_start > t) { t = effective_start; continue; }
        } else {
            if (block.block_id != prev_block_id) prog_counts[block.block_id] = 0;
            prev_block_id = block.block_id;
        }

        auto item_opt = nextItemSim(channel_id, block, state, seed);
        if (!item_opt) {
            t += 60;
            continue;
        }

        auto item = std::move(*item_opt);
        if (item.duration_ms <= 0) { t += 60; continue; }

        // Cap duration at the block's end_time boundary
        int64_t dur_ms = item.duration_ms;
        if (block.end_time.has_value()) {
            auto  tm_t    = toChannelTZ(t, tz);
            int   cur_min = tm_t.tm_hour * 60 + tm_t.tm_min;
            int   end_min = parseTimeMins(*block.end_time);
            if (end_min <= cur_min) { t += 60; continue; }
            int64_t rem = static_cast<int64_t>(end_min - cur_min) * 60000;
            dur_ms = std::min(dur_ms, rem);
        }

        item.wall_clock_start_ms = static_cast<int64_t>(t) * 1000;
        item.wall_clock_end_ms   = item.wall_clock_start_ms + item.duration_ms;
        // Snapshot the SimState after this item so the cache can resume here.
        item.cursor_json         = simStateToJson(state).dump();

        result.push_back(std::move(item));
        t += dur_ms / 1000;
        const std::time_t t_prog_end = t; // snapshot before inter-filler / alignment move t

        bool prog_limit_hit = (block.program_count > 0 &&
                               ++prog_counts[block.block_id] >= block.program_count);

        // ── Inter-filler injection ────────────────────────────────────────────
        // Only active for episode-scope aligned blocks: fills the gap between
        // the item that just ended and the next per-episode boundary. Clips can
        // nudge the next program into the early/late window rather than always
        // hard-snapping to the exact boundary; the alignment check below
        // accepts any position within those windows.
        // Individual clips are collected and then collapsed into a single merged
        // "filler" item so the preview shows one block instead of every clip.
        if (block.inter_filler && block.start_scope == "episode" && block.align_to_mins > 0) {
            const auto& pool = block.filler_entries.empty() ? channel_filler
                                                            : block.filler_entries;
            if (!pool.empty()) {
                std::time_t step   = static_cast<std::time_t>(block.align_to_mins) * 60;
                std::time_t prev_b = (t / step) * step;
                std::time_t next_b = prev_b + step;
                std::time_t gap    = next_b - t;
                std::time_t past   = t - prev_b;
                bool in_early = gap  <= static_cast<std::time_t>(block.early_start_secs);
                bool in_late  = past <= static_cast<std::time_t>(block.late_start_mins) * 60;

                if (!in_early && !in_late) {
                    const std::time_t fill_target   = next_b;
                    const std::time_t late_boundary = next_b + static_cast<std::time_t>(block.late_start_mins) * 60;
                    const std::size_t filler_start  = result.size();
                    std::string       last_cursor    = simStateToJson(state).dump();

                    while (t < fill_target && static_cast<int>(result.size()) < MAX_ITEMS) {
                        int64_t remaining_ms = (fill_target - t) * 1000;
                        // A clip is only rejected if it would push the NEXT episode
                        // start past the late-start window entirely.  Using remaining_ms
                        // here (not late_boundary) was causing premature loop exit when
                        // sequential/shuffle returned a clip slightly longer than the gap.
                        int64_t max_clip_ms  = (late_boundary - t) * 1000;

                        auto fi = pickFillerSim(channel_id, block, pool, remaining_ms, state, seed);
                        if (!fi || fi->duration_ms <= 0) break;
                        if (fi->duration_ms > max_clip_ms) break;

                        fi->wall_clock_start_ms = static_cast<int64_t>(t) * 1000;
                        fi->wall_clock_end_ms   = fi->wall_clock_start_ms + fi->duration_ms;
                        last_cursor             = simStateToJson(state).dump();
                        fi->cursor_json         = last_cursor;
                        result.push_back(std::move(*fi));
                        t += fi->duration_ms / 1000;

                        // Stop once we've entered the early or late window.
                        std::time_t prev_b2 = (t / step) * step;
                        std::time_t next_b2 = prev_b2 + step;
                        bool now_early = (next_b2 - t) <= static_cast<std::time_t>(block.early_start_secs);
                        bool now_late  = (t - prev_b2) <= static_cast<std::time_t>(block.late_start_mins) * 60;
                        if (now_early || now_late) break;
                    }

                    // Merge all collected clips into a single "filler" block for the preview.
                    if (result.size() > filler_start) {
                        int64_t merged_start = result[filler_start].wall_clock_start_ms;
                        int64_t merged_end   = result.back().wall_clock_end_ms;
                        result.erase(result.begin() + static_cast<std::ptrdiff_t>(filler_start),
                                     result.end());
                        ScheduledItem merged;
                        merged.item_type          = "filler";
                        merged.channel_id         = channel_id;
                        merged.block_id           = block.block_id;
                        merged.wall_clock_start_ms = merged_start;
                        merged.wall_clock_end_ms   = merged_end;
                        merged.duration_ms         = merged_end - merged_start;
                        merged.cursor_json         = last_cursor;
                        merged.is_filler           = true;
                        result.push_back(std::move(merged));
                    }
                }
            }
        }

        // Episode-scope: apply alignment + early/late window after every item.
        if (block.start_scope == "episode" && block.align_to_mins > 0) {
            std::time_t step   = static_cast<std::time_t>(block.align_to_mins) * 60;
            std::time_t prev_b = (t / step) * step;        // last boundary at or before t
            std::time_t next_b = prev_b + step;            // next boundary
            std::time_t past   = t - prev_b;               // seconds past last boundary
            std::time_t gap    = next_b - t;               // seconds until next boundary

            bool in_early = gap  <= static_cast<std::time_t>(block.early_start_secs);
            bool in_late  = past <= static_cast<std::time_t>(block.late_start_mins) * 60;

            if (!in_early && !in_late) {
                // Outside both windows — snap to next boundary
                t = next_b;
            }
            // in_early (close enough to next mark) or in_late (just past last mark): start immediately
        }

        // ── program_count stop condition ──────────────────────────────────────
        // Snap t to the next local-time alignment boundary computed from
        // t_prog_end (the episode-end time *before* inter-filler / episode-scope
        // alignment ran). Using t_prog_end prevents a 1-second filler overshoot
        // from causing a full boundary jump (e.g. 19:00→19:30) that would leave
        // a 30-minute gap before the first episode of the underlying block.
        if (prog_limit_hit) {
            if (block.align_to_mins > 0) {
                auto        tm_end   = toChannelTZ(t_prog_end, tz);
                int         cur_sec  = tm_end.tm_hour * 3600 + tm_end.tm_min * 60 + tm_end.tm_sec;
                std::time_t midnight = t_prog_end - static_cast<std::time_t>(cur_sec);
                std::time_t step     = static_cast<std::time_t>(block.align_to_mins) * 60;
                t = midnight + ((static_cast<std::time_t>(cur_sec) + step - 1) / step) * step;
            }
            exhausted_blocks.insert(block.block_id);
            prev_block_id.clear();
        }
    }
    return result;
}

// ── Playback completion ───────────────────────────────────────────────────────

void RuleEngine::markPlayed(const std::string& channel_id, const std::string& block_id,
                              const std::string& item_type, const std::string& item_id,
                              int64_t /*duration_ms*/) {
    SQLite::Transaction txn(db_.get());

    // Record history
    SQLite::Statement qh(db_.get(), R"(
        INSERT INTO play_history (item_type, item_id, channel_id, block_id, aired_at)
        VALUES (?,?,?,?,?)
    )");
    qh.bind(1, item_type); qh.bind(2, item_id); qh.bind(3, channel_id);
    if (block_id.empty()) qh.bind(4); else qh.bind(4, block_id);
    qh.bind(5, static_cast<int64_t>(std::time(nullptr)));
    qh.exec();

    // Advance cursor
    if (!block_id.empty()) {
        auto blocks = loadBlocks(channel_id);
        for (const auto& b : blocks) {
            if (b.block_id != block_id) continue;
            if (b.content.empty()) break;

            int n  = static_cast<int>(b.content.size());
            int rr = readBlockRR(block_id, channel_id) % n;
            const auto& bc = b.content[rr];

            if (bc.content_type == "show") {
                if (isRerunMode(b.advancement)) {
                    // ── Rerun cursor advance + run tracking ──────────────────
                    int runs_rem = readRunsRemaining(b.block_id, channel_id);

                    // Advance the episode cursor for the currently-selected show.
                    auto eps = (b.advancement == Advancement::RerunSmart)
                        ? getPlayedEpisodesWithCooldown(bc.content_id, channel_id, bc.season_filter, b.smart_pct)
                        : getPlayedEpisodes(bc.content_id, channel_id, bc.season_filter);
                    if (!eps.empty()) {
                        int pos      = readCursorPos("show_rerun", bc.content_id, "block", b.block_id);
                        int next_pos = (pos + 1) % static_cast<int>(eps.size());
                        writeCursorPos("show_rerun", bc.content_id, "block", b.block_id,
                                       next_pos, eps[next_pos].episode_id);
                    } else if (b.no_history_behavior == NoHistoryBehavior::Normal) {
                        // No play history: advance the regular show cursor instead.
                        auto all = getEpisodes(bc.content_id, bc.season_filter);
                        if (!all.empty()) {
                            std::string scope    = scopeStr(b);
                            std::string scope_id = scopeId(b, channel_id);
                            int pos      = readCursorPos("show", bc.content_id, scope, scope_id);
                            int next_pos = (pos + 1) % static_cast<int>(all.size());
                            writeCursorPos("show", bc.content_id, scope, scope_id,
                                           next_pos, all[next_pos].episode_id);
                        }
                    } else if (b.no_history_behavior == NoHistoryBehavior::FallbackAll) {
                        // Full catalog treated as rerun pool: advance the show_rerun cursor.
                        auto all = getEpisodes(bc.content_id, bc.season_filter);
                        if (!all.empty()) {
                            int pos      = readCursorPos("show_rerun", bc.content_id, "block", b.block_id);
                            int next_pos = (pos + 1) % static_cast<int>(all.size());
                            writeCursorPos("show_rerun", bc.content_id, "block", b.block_id,
                                           next_pos, all[next_pos].episode_id);
                        }
                    }

                    if (runs_rem <= 1) {
                        // Run complete — pick next show (weighted random) and set its starting position.
                        std::mt19937_64 rng{std::random_device{}()};
                        int next_sel;
                        if (b.no_history_behavior == NoHistoryBehavior::Exclude) {
                            // Select only from shows that have play history.
                            int cnt = static_cast<int>(b.content.size());
                            std::vector<int> eligible;
                            int total_w = 0;
                            for (int i = 0; i < cnt; i++) {
                                const auto& cbc = b.content[i];
                                if (cbc.content_type == "show") {
                                    auto ceps = (b.advancement == Advancement::RerunSmart)
                                        ? getPlayedEpisodesWithCooldown(cbc.content_id, channel_id, cbc.season_filter, b.smart_pct)
                                        : getPlayedEpisodes(cbc.content_id, channel_id, cbc.season_filter);
                                    if (ceps.empty()) continue;
                                }
                                eligible.push_back(i);
                                total_w += std::max(1, cbc.weight);
                            }
                            if (eligible.empty()) {
                                writeRerunState(b.block_id, channel_id, 0, 0);
                                break;
                            }
                            std::uniform_int_distribution<int> dist(0, total_w - 1);
                            int r = dist(rng);
                            next_sel = eligible.back();
                            for (int idx : eligible) {
                                r -= std::max(1, b.content[idx].weight);
                                if (r < 0) { next_sel = idx; break; }
                            }
                        } else {
                            next_sel = selectWeighted(b, rng);
                        }
                        const auto& next_bc = b.content[next_sel];
                        auto next_eps = (b.advancement == Advancement::RerunSmart)
                            ? getPlayedEpisodesWithCooldown(next_bc.content_id, channel_id, next_bc.season_filter, b.smart_pct)
                            : getPlayedEpisodes(next_bc.content_id, channel_id, next_bc.season_filter);
                        if (next_eps.empty() && b.no_history_behavior == NoHistoryBehavior::FallbackAll)
                            next_eps = getEpisodes(next_bc.content_id, next_bc.season_filter);
                        // For Normal: show_rerun cursor is irrelevant until the show gains history;
                        // leave it at 0 so it starts from the first played episode when it does.
                        if (!next_eps.empty()) {
                            std::uniform_int_distribution<int> dist(0, static_cast<int>(next_eps.size()) - 1);
                            int start = dist(rng);
                            int snap  = snapToGroupStart(next_eps[start].episode_id, next_eps);
                            int final_start = (snap >= 0) ? snap : start;
                            writeCursorPos("show_rerun", next_bc.content_id, "block", b.block_id,
                                           final_start, next_eps[final_start].episode_id);
                        }
                        int next_run_count = std::max(1, next_bc.run_count);
                        writeRerunState(b.block_id, channel_id, next_sel, next_run_count);
                    } else {
                        writeRerunState(b.block_id, channel_id,
                                        readBlockRR(b.block_id, channel_id), runs_rem - 1);
                    }
                } else {
                    auto eps = getEpisodes(bc.content_id, bc.season_filter);
                    if (!eps.empty()) {
                        std::string scope    = scopeStr(b);
                        std::string scope_id = scopeId(b, channel_id);
                        int pos      = readCursorPos("show", bc.content_id, scope, scope_id);
                        // Shuffle/SmartShuffle: position grows monotonically (epoch = pos/n).
                        // Sequential: position wraps.
                        int next_pos;
                        if (b.advancement == Advancement::Shuffle ||
                            b.advancement == Advancement::SmartShuffle) {
                            next_pos = pos + 1;
                        } else {
                            next_pos = (pos + 1) % static_cast<int>(eps.size());
                        }
                        std::string ep_id = eps[next_pos % static_cast<int>(eps.size())].episode_id;
                        writeCursorPos("show", bc.content_id, scope, scope_id, next_pos, ep_id);
                    }
                }
            } else if (bc.content_type == "playlist" || bc.content_type == "filler_list") {
                bool is_fl = (bc.content_type == "filler_list");
                const char* cnt_sql = is_fl
                    ? "SELECT COUNT(*) FROM filler_list_item WHERE filler_list_id=?"
                    : "SELECT COUNT(*) FROM playlist_item     WHERE playlist_id=?";
                SQLite::Statement q(db_.get(), cnt_sql);
                q.bind(1, bc.content_id);
                if (q.executeStep()) {
                    int list_size = q.getColumn(0).getInt();
                    if (list_size > 0) {
                        std::string scope    = scopeStr(b);
                        std::string scope_id = scopeId(b, channel_id);
                        int pos      = readCursorPos(bc.content_type, bc.content_id, scope, scope_id);
                        int next_pos = (pos + 1) % list_size;
                        writeCursorPos(bc.content_type, bc.content_id, scope, scope_id, next_pos);
                    }
                }
            }
            writeBlockRR(block_id, channel_id, (rr + 1) % n);
            break;
        }
    }

    txn.commit();
}
