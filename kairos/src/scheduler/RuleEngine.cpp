#include "RuleEngine.h"
#include "../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <unordered_set>

#include "RuntimeFlags.h"

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

        year_month_day jan1{ymd.year(), month{1}, day{1}};
        int yday = static_cast<int>((sys_days{ymd} - sys_days{jan1}).count());

        std::tm tm{};
        tm.tm_year = int(ymd.year()) - 1900;
        tm.tm_mon  = unsigned(ymd.month()) - 1;
        tm.tm_mday = unsigned(ymd.day());
        tm.tm_hour = hour;
        tm.tm_min  = min;
        tm.tm_sec  = sec;
        tm.tm_wday = wday;
        tm.tm_yday = yday;
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
    if (s == "skip")         return NoHistoryBehavior::Skip;
    return NoHistoryBehavior::Normal;
}

// ── SimState JSON serialization (filler-only) ────────────────────────────────
// Content cursor state is entirely in the DB; only filler round-robin positions
// need in-memory tracking across a single project() call.

static json simStateToJson(const RuleEngine::SimState& s) {
    json j;
    j["show_pos"] = json::object();
    for (const auto& [k, v] : s.show_pos) j["show_pos"][k] = v;
    return j;
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
               filler_selection, smart_pct, start_scope, no_history_behavior,
               max_consecutive_episodes,
               intro_content_type, intro_content_id,
               outro_content_type, outro_content_id,
               interstitial_content_type, interstitial_content_id, interstitial_every_n,
               snap_to_group_start
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
        b.smart_pct                = q.getColumn(15).getInt();
        b.start_scope              = q.getColumn(16).getString();
        b.no_history_behavior      = parseNoHistoryBehavior(q.getColumn(17).getString());
        b.max_consecutive_episodes      = q.getColumn(18).getInt();
        b.intro_content_type            = q.getColumn(19).getString();
        b.intro_content_id              = q.getColumn(20).getString();
        b.outro_content_type            = q.getColumn(21).getString();
        b.outro_content_id              = q.getColumn(22).getString();
        b.interstitial_content_type     = q.getColumn(23).getString();
        b.interstitial_content_id       = q.getColumn(24).getString();
        b.interstitial_every_n          = q.getColumn(25).getInt();
        b.snap_to_group_start           = q.getColumn(26).getInt() != 0;

        SQLite::Statement cq(db_.get(), R"(
            SELECT id, content_type, content_id, position, season_filter, weight, run_count,
                   include_specials, episode_order
            FROM block_content WHERE block_id = ? ORDER BY position
        )");
        cq.bind(1, b.block_id);
        while (cq.executeStep()) {
            BlockContent bc;
            bc.id              = cq.getColumn(0).getInt();
            bc.block_id        = b.block_id;
            bc.content_type    = cq.getColumn(1).getString();
            bc.content_id      = cq.getColumn(2).getString();
            bc.position        = cq.getColumn(3).getInt();
            if (!cq.getColumn(4).isNull())
                bc.season_filter = cq.getColumn(4).getInt();
            bc.weight           = cq.getColumn(5).getInt();
            bc.run_count        = cq.getColumn(6).getInt();
            bc.include_specials = cq.getColumn(7).getInt() != 0;
            bc.episode_order    = cq.getColumn(8).getString();
            b.content.push_back(std::move(bc));
        }

        SQLite::Statement fq(db_.get(), R"(
            SELECT content_type, content_id, advancement, weight, season_filter
            FROM block_filler_entry WHERE block_id = ? ORDER BY position
        )");
        fq.bind(1, b.block_id);
        while (fq.executeStep()) {
            BlockFillerEntry fe;
            fe.content_type = fq.getColumn(0).getString();
            fe.content_id   = fq.getColumn(1).getString();
            fe.advancement  = fq.getColumn(2).getString();
            fe.weight       = fq.getColumn(3).getInt();
            if (!fq.getColumn(4).isNull()) fe.season_filter = fq.getColumn(4).getInt();
            b.filler_entries.push_back(std::move(fe));
        }

        blocks.push_back(std::move(b));
    }
    return blocks;
}

std::vector<Episode> RuleEngine::getEpisodes(const std::string& show_id,
                                              std::optional<int> season,
                                              bool include_specials,
                                              const std::string& episode_order) {
    std::string sql =
        "SELECT episode_id, show_id, season, episode, title, file_path, duration_ms,"
        " overview, air_date, thumb"
        " FROM episode WHERE show_id = ?";
    if (season)                         sql += " AND season = ?";
    if (!season && !include_specials)   sql += " AND season != 0";
    if (episode_order == "absolute")
        sql += " ORDER BY COALESCE(absolute_index, season * 10000 + episode)";
    else if (episode_order == "airdate")
        sql += " ORDER BY air_date, episode";
    else
        sql += " ORDER BY season, episode";

    SQLite::Statement q(db_.get(), sql);
    q.bind(1, show_id);
    if (season) q.bind(2, *season);

    std::vector<Episode> eps;
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
                                                     std::optional<int> season,
                                                     std::time_t before_time,
                                                     bool global_scope,
                                                     bool include_specials,
                                                     const std::string& episode_order) {
    std::string sql =
        "SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,"
        " e.duration_ms, e.overview, e.air_date, e.thumb"
        " FROM episode e WHERE e.show_id = ?";
    if (season)                       sql += " AND e.season = ?";
    if (!season && !include_specials) sql += " AND e.season != 0";
    sql += " AND EXISTS (SELECT 1 FROM play_history ph"
           " WHERE ph.item_type='episode' AND ph.item_id=e.episode_id";
    if (!global_scope) sql += " AND ph.channel_id=?";
    sql += " AND ph.aired_at < ?)";
    if (episode_order == "absolute")
        sql += " ORDER BY COALESCE(e.absolute_index, e.season * 10000 + e.episode)";
    else if (episode_order == "airdate")
        sql += " ORDER BY e.air_date, e.episode";
    else
        sql += " ORDER BY e.season, e.episode";

    SQLite::Statement q(db_.get(), sql);
    int idx = 1;
    q.bind(idx++, show_id);
    if (season) q.bind(idx++, *season);
    if (!global_scope) q.bind(idx++, channel_id);
    q.bind(idx++, static_cast<int64_t>(before_time));

    std::vector<Episode> eps;
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
                                                                  int smart_pct,
                                                                  std::time_t before_time,
                                                                  bool global_scope,
                                                                  bool include_specials) {
    // Full played pool ordered oldest→newest.
    const char* sql_ch_season = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.channel_id=? AND ph.aired_at < ?
        WHERE e.show_id=? AND e.season=?
        GROUP BY e.episode_id
        ORDER BY last_aired ASC
    )";
    const char* sql_ch_all = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.channel_id=? AND ph.aired_at < ?
        WHERE e.show_id=?
        GROUP BY e.episode_id
        ORDER BY last_aired ASC
    )";
    const char* sql_gl_season = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.aired_at < ?
        WHERE e.show_id=? AND e.season=?
        GROUP BY e.episode_id
        ORDER BY last_aired ASC
    )";
    const char* sql_gl_all = R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title, e.file_path,
               e.duration_ms, e.overview, e.air_date, e.thumb,
               MAX(ph.aired_at) AS last_aired
        FROM episode e
        JOIN play_history ph ON ph.item_type='episode' AND ph.item_id=e.episode_id
                             AND ph.aired_at < ?
        WHERE e.show_id=?
        GROUP BY e.episode_id
        ORDER BY last_aired ASC
    )";

    SQLite::Statement q(db_.get(), global_scope
        ? (season ? sql_gl_season : sql_gl_all)
        : (season ? sql_ch_season : sql_ch_all));
    if (global_scope) {
        q.bind(1, static_cast<int64_t>(before_time)); q.bind(2, show_id);
        if (season) q.bind(3, *season);
    } else {
        q.bind(1, channel_id); q.bind(2, static_cast<int64_t>(before_time)); q.bind(3, show_id);
        if (season) q.bind(4, *season);
    }

    std::vector<Episode> all;
    while (q.executeStep()) {
        if (!include_specials && !season && q.getColumn(2).getInt() == 0) continue;
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
    Xoshiro256 rng(std::hash<std::string>{}(seed_str));
    std::shuffle(order.begin(), order.end(), rng);
    return order;
}

std::vector<int> RuleEngine::groupedShufflePermutation(const std::string& seed_str,
                                                        const std::vector<Episode>& eps) const {
    if (eps.empty()) return {};

    // Fetch multipart group membership for this show in one query.
    SQLite::Statement q(db_.get(), R"(
        SELECT egm.episode_id, egm.group_id, egm.part_num
        FROM episode_group_member egm
        JOIN episode_group eg ON eg.group_id = egm.group_id
        WHERE eg.show_id = ?
    )");
    q.bind(1, eps[0].show_id);

    std::unordered_map<std::string, std::pair<std::string, int>> ep_group; // ep_id → {group_id, part_num}
    while (q.executeStep())
        ep_group[q.getColumn(0).getString()] = {q.getColumn(1).getString(), q.getColumn(2).getInt()};

    // Build chunks: episodes in a multipart group become one chunk (ordered by part_num).
    // Episodes with no group membership are singletons.
    std::unordered_map<std::string, std::vector<std::pair<int, int>>> groups; // group_id → [(eps_idx, part_num)]
    std::vector<std::vector<int>> chunks;

    for (int i = 0; i < static_cast<int>(eps.size()); ++i) {
        auto it = ep_group.find(eps[i].episode_id);
        if (it != ep_group.end())
            groups[it->second.first].emplace_back(i, it->second.second);
        else
            chunks.push_back({i});
    }

    for (auto& [gid, members] : groups) {
        std::sort(members.begin(), members.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        std::vector<int> chunk;
        for (auto& [idx, part] : members) chunk.push_back(idx);
        chunks.push_back(std::move(chunk));
    }

    // Shuffle the chunks as atomic units then flatten.
    Xoshiro256 rng(std::hash<std::string>{}(seed_str));
    std::shuffle(chunks.begin(), chunks.end(), rng);

    std::vector<int> perm;
    perm.reserve(eps.size());
    for (auto& chunk : chunks)
        for (int idx : chunk) perm.push_back(idx);
    return perm;
}

int RuleEngine::selectWeighted(const Block& block, Xoshiro256& rng) {
    if (block.content.empty()) return 0;
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

int RuleEngine::selectWeightedSmartCooldown(
    const Block& block, const std::string& channel_id,
    int smart_pct, std::time_t before_time, Xoshiro256& rng)
{
    int n = static_cast<int>(block.content.size());
    if (n <= 1 || smart_pct <= 0) return selectWeighted(block, rng);

    // Only apply movie-level cooldown when all content entries are movies.
    for (const auto& bc : block.content)
        if (bc.content_type != "movie") return selectWeighted(block, rng);

    int hot_count = std::max(0, n * smart_pct / 100);
    if (hot_count == 0) return selectWeighted(block, rng);

    // Most recently played movies for this channel (ordered most-recent first).
    std::unordered_set<std::string> hot_ids;
    {
        SQLite::Statement q(db_.get(), R"(
            SELECT item_id FROM (
                SELECT item_id, MAX(aired_at) AS last_aired
                FROM play_history
                WHERE item_type='movie' AND channel_id=? AND aired_at<?
                GROUP BY item_id
            ) ORDER BY last_aired DESC LIMIT ?
        )");
        q.bind(1, channel_id);
        q.bind(2, static_cast<int64_t>(before_time));
        q.bind(3, hot_count);
        while (q.executeStep()) hot_ids.insert(q.getColumn(0).getString());
    }
    if (hot_ids.empty()) return selectWeighted(block, rng);

    int total = 0;
    for (const auto& bc : block.content)
        if (!hot_ids.count(bc.content_id)) total += std::max(1, bc.weight);
    if (total <= 0) return selectWeighted(block, rng); // all hot — fall back

    std::uniform_int_distribution<int> dist(0, total - 1);
    int r = dist(rng);
    for (int i = 0; i < n; ++i) {
        if (hot_ids.count(block.content[i].content_id)) continue;
        r -= std::max(1, block.content[i].weight);
        if (r < 0) return i;
    }
    return selectWeighted(block, rng);
}

std::vector<Episode> RuleEngine::smartShufflePool(
    const std::vector<Episode>& all,
    const std::string& show_id,
    const std::string& channel_id,
    int smart_pct,
    std::time_t before_time)
{
    int n = static_cast<int>(all.size());
    int hot_count = std::max(0, n * smart_pct / 100);
    if (hot_count == 0 || all.empty()) return all;

    std::unordered_set<std::string> hot_ids;
    {
        SQLite::Statement q(db_.get(), R"(
            SELECT item_id FROM (
                SELECT ph.item_id, MAX(ph.aired_at) AS last_aired
                FROM play_history ph
                JOIN episode e ON e.episode_id = ph.item_id
                WHERE ph.item_type='episode' AND ph.channel_id=? AND ph.aired_at<? AND e.show_id=?
                GROUP BY ph.item_id
            ) ORDER BY last_aired DESC LIMIT ?
        )");
        q.bind(1, channel_id);
        q.bind(2, static_cast<int64_t>(before_time));
        q.bind(3, show_id);
        q.bind(4, hot_count);
        while (q.executeStep()) hot_ids.insert(q.getColumn(0).getString());
    }
    if (hot_ids.empty()) return all;

    std::vector<Episode> filtered;
    filtered.reserve(all.size());
    for (const auto& e : all)
        if (!hot_ids.count(e.episode_id)) filtered.push_back(e);

    return filtered.empty() ? all : filtered;
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

int RuleEngine::readConsecutiveCount(const std::string& block_id,
                                      const std::string& channel_id) {
    SQLite::Statement q(db_.get(),
        "SELECT consecutive_count FROM block_state WHERE block_id=? AND channel_id=?");
    q.bind(1, block_id); q.bind(2, channel_id);
    if (q.executeStep()) return q.getColumn(0).getInt();
    return 0;
}

void RuleEngine::writeRerunState(const std::string& block_id,
                                  const std::string& channel_id,
                                  int content_pos, int runs_remaining,
                                  int consecutive_count) {
    SQLite::Statement q(db_.get(), R"(
        INSERT INTO block_state (block_id, channel_id, content_position, runs_remaining,
                                 consecutive_count, updated_at)
        VALUES (?,?,?,?,?,?)
        ON CONFLICT(block_id, channel_id)
        DO UPDATE SET content_position=excluded.content_position,
                      runs_remaining=excluded.runs_remaining,
                      consecutive_count=excluded.consecutive_count,
                      updated_at=excluded.updated_at
    )");
    q.bind(1, block_id); q.bind(2, channel_id); q.bind(3, content_pos);
    q.bind(4, runs_remaining); q.bind(5, consecutive_count);
    q.bind(6, static_cast<int64_t>(std::time(nullptr)));
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

std::vector<std::pair<std::string, std::string>>
RuleEngine::loadListItems(const std::string& content_type, const std::string& content_id) {
    const char* sql = (content_type == "filler_list")
        ? "SELECT item_type, item_id FROM filler_list_item WHERE filler_list_id=? ORDER BY position"
        : "SELECT item_type, item_id FROM playlist_item     WHERE playlist_id=?    ORDER BY position";
    SQLite::Statement q(db_.get(), sql);
    q.bind(1, content_id);
    std::vector<std::pair<std::string, std::string>> items;
    while (q.executeStep())
        items.emplace_back(q.getColumn(0).getString(), q.getColumn(1).getString());
    return items;
}

std::string RuleEngine::getPlaylistMode(const std::string& playlist_id) {
    SQLite::Statement q(db_.get(), "SELECT mode FROM playlist WHERE playlist_id=?");
    q.bind(1, playlist_id);
    if (q.executeStep()) return q.getColumn(0).getString();
    return "sequential";
}

std::vector<std::string> RuleEngine::getPlaylistShows(const std::string& playlist_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT e.show_id
        FROM playlist_item pi
        JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
        WHERE pi.playlist_id = ?
        GROUP BY e.show_id
        ORDER BY MIN(pi.position)
    )");
    q.bind(1, playlist_id);
    std::vector<std::string> shows;
    while (q.executeStep()) shows.push_back(q.getColumn(0).getString());
    return shows;
}

std::vector<Episode> RuleEngine::getPlaylistShowEpisodes(const std::string& playlist_id,
                                                          const std::string& show_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode, e.title,
               e.file_path, e.duration_ms, e.overview, e.air_date, e.thumb
        FROM playlist_item pi
        JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
        WHERE pi.playlist_id = ? AND e.show_id = ?
        ORDER BY pi.position
    )");
    q.bind(1, playlist_id); q.bind(2, show_id);
    std::vector<Episode> eps;
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

std::optional<ScheduledItem> RuleEngine::episodeById(const std::string& episode_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT e.episode_id, e.show_id, e.season, e.episode,
               e.title, e.file_path, e.duration_ms, s.title
        FROM episode e LEFT JOIN show s ON s.show_id = e.show_id
        WHERE e.episode_id=?
    )");
    q.bind(1, episode_id);
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
    return item;
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

std::string RuleEngine::channelAdvanceMode(const std::string& channel_id) {
    SQLite::Statement q(db_.get(), "SELECT advance_mode FROM channel WHERE channel_id=?");
    q.bind(1, channel_id);
    if (q.executeStep()) {
        auto m = q.getColumn(0).getString();
        if (!m.empty()) return m;
    }
    return "scheduled";
}

std::optional<Block> RuleEngine::resolveBlock(const std::string& channel_id, std::time_t t) {
    auto blocks = loadBlocks(channel_id);
    return resolveFromList(blocks, t, channelTimezone(channel_id));
}

// ── Item selection ────────────────────────────────────────────────────────────

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

static ScheduledItem movieItem(const Movie& m) {
    ScheduledItem item;
    item.item_type   = "movie";
    item.item_id     = m.movie_id;
    item.file_path   = m.file_path;
    item.duration_ms = m.duration_ms;
    item.title       = m.title;
    return item;
}

std::optional<ScheduledItem> RuleEngine::nextItem(const std::string& channel_id,
                                                    const Block& block,
                                                    std::time_t before_time) {
    if (block.content.empty()) return std::nullopt;

    int  n  = static_cast<int>(block.content.size());
    int  rr = readBlockRR(block.block_id, channel_id) % n;
    const auto& bc = block.content[rr];

    if (bc.content_type == "show") {
        if (isRerunMode(block.advancement)) {
            // Rerun: content_position in block_state holds the selected show index.
            bool global = (block.cursor_scope == CursorScope::Global);
            auto eps = (block.advancement == Advancement::RerunSmart)
                ? getPlayedEpisodesWithCooldown(bc.content_id, channel_id, bc.season_filter, block.smart_pct, before_time, global, bc.include_specials)
                : getPlayedEpisodes(bc.content_id, channel_id, bc.season_filter, before_time, global, bc.include_specials, bc.episode_order);
            if (block.no_history_behavior == NoHistoryBehavior::Normal) {
                auto all = getEpisodes(bc.content_id, bc.season_filter, bc.include_specials, bc.episode_order);
                if (all.empty()) return std::nullopt;
                int seq_pos = readCursorPos("show", bc.content_id,
                                            scopeStr(block), scopeId(block, channel_id));
                if (block.advancement == Advancement::RerunSmart) {
                    // RerunSmart: random entry into full catalog, sequential within run.
                    // "show" cursor holds the random start and is advanced +1 per episode.
                    return itemFromShow(channel_id, block.block_id, all, seq_pos, showTitle(bc.content_id));
                }
                // RerunShuffle: play sequentially until all episodes have aired once
                // (seq_pos >= pool size), then switch to rerun shuffle within played pool.
                if (eps.empty() || seq_pos >= static_cast<int>(eps.size()))
                    return itemFromShow(channel_id, block.block_id, all, seq_pos, showTitle(bc.content_id));
                // All episodes seen at least once — fall through to rerun shuffle below.
            } else if (eps.empty()) {
                switch (block.no_history_behavior) {
                    case NoHistoryBehavior::FallbackAll:
                        eps = getEpisodes(bc.content_id, bc.season_filter, bc.include_specials, bc.episode_order);
                        if (eps.empty()) return std::nullopt;
                        break;
                    default:
                        return std::nullopt;
                }
            }
            int pos  = readCursorPos("show_rerun", bc.content_id, scopeStr(block), scopeId(block, channel_id));
            auto perm = groupedShufflePermutation(bc.content_id + block.block_id, eps);
            return itemFromShow(channel_id, block.block_id, eps,
                                perm[pos % static_cast<int>(perm.size())],
                                showTitle(bc.content_id));
        }
        auto all_eps = getEpisodes(bc.content_id, bc.season_filter, bc.include_specials, bc.episode_order);
        if (all_eps.empty()) return std::nullopt;
        int pos = readCursorPos("show", bc.content_id, scopeStr(block), scopeId(block, channel_id));
        if (block.advancement == Advancement::SmartShuffle && block.smart_pct > 0) {
            auto eps = smartShufflePool(all_eps, bc.content_id, channel_id, block.smart_pct, before_time);
            auto perm = shufflePermutation(bc.content_id, static_cast<int>(eps.size()));
            return itemFromShow(channel_id, block.block_id, eps,
                                perm[pos % static_cast<int>(perm.size())], showTitle(bc.content_id));
        }
        if (block.advancement == Advancement::Shuffle) {
            int epoch = pos / static_cast<int>(all_eps.size());
            int idx   = pos % static_cast<int>(all_eps.size());
            auto perm = shufflePermutation(bc.content_id + std::to_string(epoch), static_cast<int>(all_eps.size()));
            return itemFromShow(channel_id, block.block_id, all_eps, perm[idx], showTitle(bc.content_id));
        }
        return itemFromShow(channel_id, block.block_id, all_eps, pos, showTitle(bc.content_id));
    }
    if (bc.content_type == "movie") {
        auto m = getMovie(bc.content_id);
        if (!m) return std::nullopt;
        auto item = movieItem(*m);
        item.channel_id = channel_id;
        item.block_id   = block.block_id;
        return item;
    }
    if (bc.content_type == "episode") {
        auto item = episodeById(bc.content_id);
        if (!item) return std::nullopt;
        item->channel_id = channel_id;
        item->block_id   = block.block_id;
        return item;
    }
    if (bc.content_type == "playlist" || bc.content_type == "filler_list") {
        // show_collection: treat distinct shows inside the playlist as the show pool
        if (bc.content_type == "playlist" && getPlaylistMode(bc.content_id) == "show_collection") {
            auto shows = getPlaylistShows(bc.content_id);
            if (shows.empty()) return std::nullopt;
            int n_shows = static_cast<int>(shows.size());
            std::string scope    = scopeStr(block);
            std::string scope_id = scopeId(block, channel_id);
            int show_idx = readCursorPos("playlist", bc.content_id, scope, scope_id) % n_shows;
            const std::string& show_id = shows[show_idx];

            if (isRerunMode(block.advancement)) {
                bool global = (block.cursor_scope == CursorScope::Global);
                auto eps = (block.advancement == Advancement::RerunSmart)
                    ? getPlayedEpisodesWithCooldown(show_id, channel_id, std::nullopt, block.smart_pct, before_time, global)
                    : getPlayedEpisodes(show_id, channel_id, std::nullopt, before_time, global);
                if (eps.empty()) {
                    switch (block.no_history_behavior) {
                        case NoHistoryBehavior::Normal: {
                            auto pl_eps = getPlaylistShowEpisodes(bc.content_id, show_id);
                            if (pl_eps.empty()) return std::nullopt;
                            int pos = readCursorPos("show", show_id, scope, scope_id);
                            return itemFromShow(channel_id, block.block_id, pl_eps, pos, showTitle(show_id));
                        }
                        case NoHistoryBehavior::FallbackAll:
                            eps = getEpisodes(show_id, std::nullopt);
                            if (eps.empty()) return std::nullopt;
                            break;
                        default:
                            return std::nullopt;
                    }
                }
                int pos  = readCursorPos("show_rerun", show_id, scope, scope_id);
                auto perm = groupedShufflePermutation(show_id + block.block_id, eps);
                return itemFromShow(channel_id, block.block_id, eps,
                                    perm[pos % static_cast<int>(perm.size())],
                                    showTitle(show_id));
            }
            auto pl_eps = getPlaylistShowEpisodes(bc.content_id, show_id);
            if (pl_eps.empty()) return std::nullopt;
            int pos = readCursorPos("show", show_id, scope, scope_id);
            if (block.advancement == Advancement::SmartShuffle && block.smart_pct > 0) {
                auto eps = smartShufflePool(pl_eps, show_id, channel_id, block.smart_pct, before_time);
                auto perm = shufflePermutation(show_id + block.block_id, static_cast<int>(eps.size()));
                return itemFromShow(channel_id, block.block_id, eps,
                                    perm[pos % static_cast<int>(perm.size())], showTitle(show_id));
            }
            if (block.advancement == Advancement::Shuffle) {
                int epoch = pos / static_cast<int>(pl_eps.size());
                int idx   = pos % static_cast<int>(pl_eps.size());
                auto perm = shufflePermutation(show_id + block.block_id + std::to_string(epoch),
                                               static_cast<int>(pl_eps.size()));
                return itemFromShow(channel_id, block.block_id, pl_eps, perm[idx], showTitle(show_id));
            }
            return itemFromShow(channel_id, block.block_id, pl_eps, pos, showTitle(show_id));
        }

        // sequential playlist / filler_list: play items in flat order
        auto items = loadListItems(bc.content_type, bc.content_id);
        if (items.empty()) return std::nullopt;

        int pos = readCursorPos(bc.content_type, bc.content_id, scopeStr(block), scopeId(block, channel_id));
        pos = pos % static_cast<int>(items.size());
        const auto& [ptype, pid] = items[pos];

        if (ptype == "episode") {
            auto item = episodeById(pid);
            if (!item) return std::nullopt;
            item->channel_id = channel_id;
            item->block_id   = block.block_id;
            return item;
        } else {
            auto m = getMovie(pid);
            if (!m) return std::nullopt;
            auto item = movieItem(*m);
            item.channel_id = channel_id;
            item.block_id   = block.block_id;
            return item;
        }
    }
    return std::nullopt;
}

// ── Cursor advance after scheduling or confirming one item ───────────────────
// Mirrors the advance logic in markPlayed but takes the Block directly so the
// caller (project()) avoids reloading all blocks for every scheduled item.

void RuleEngine::advanceCursors(const std::string& channel_id, const Block& b,
                                 std::time_t before_time, Xoshiro256& rng) {
    if (b.content.empty()) return;

    int n  = static_cast<int>(b.content.size());
    int rr = readBlockRR(b.block_id, channel_id) % n;
    const auto& bc = b.content[rr];

    if (bc.content_type == "show") {
        if (isRerunMode(b.advancement)) {
            bool global = (b.cursor_scope == CursorScope::Global);
            std::string rr_scope    = scopeStr(b);
            std::string rr_scope_id = scopeId(b, channel_id);
            auto eps = (b.advancement == Advancement::RerunSmart)
                ? getPlayedEpisodesWithCooldown(bc.content_id, channel_id, bc.season_filter, b.smart_pct, before_time, global, bc.include_specials)
                : getPlayedEpisodes(bc.content_id, channel_id, bc.season_filter, before_time, global, bc.include_specials, bc.episode_order);
            if (b.no_history_behavior == NoHistoryBehavior::Normal) {
                auto all = getEpisodes(bc.content_id, bc.season_filter, bc.include_specials, bc.episode_order);
                if (!all.empty()) {
                    std::string scope    = scopeStr(b);
                    std::string scope_id = scopeId(b, channel_id);
                    int seq_pos = readCursorPos("show", bc.content_id, scope, scope_id);
                    if (b.advancement == Advancement::RerunSmart) {
                        // RerunSmart: sequential within run; random start set on show selection.
                        // No premiers (eps empty): free advance — group follow-through can cross
                        //   into unaired episodes (hook: Part 1 rerun before Part 2 premieres).
                        // Has premiers (eps non-empty): cap at pool boundary so we never advance
                        //   into unaired episodes on premier-constrained shows.
                        int next_pos = (seq_pos + 1) % static_cast<int>(all.size());
                        if (!eps.empty()) {
                            std::unordered_set<std::string> aired;
                            aired.reserve(eps.size());
                            for (const auto& e : eps) aired.insert(e.episode_id);
                            if (!aired.count(all[next_pos].episode_id)) next_pos = 0;
                        }
                        writeCursorPos("show", bc.content_id, scope, scope_id,
                                       next_pos, all[next_pos].episode_id);
                    } else if (eps.empty() || seq_pos >= static_cast<int>(eps.size())) {
                        // RerunShuffle still in first-run: advance sequential cursor.
                        int next_pos = (seq_pos + 1) % static_cast<int>(all.size());
                        writeCursorPos("show", bc.content_id, scope, scope_id,
                                       next_pos, all[next_pos].episode_id);
                    } else {
                        // RerunShuffle first-run complete: advance rerun cursor within pool.
                        int pos      = readCursorPos("show_rerun", bc.content_id, rr_scope, rr_scope_id);
                        int next_pos = (pos + 1) % static_cast<int>(eps.size());
                        writeCursorPos("show_rerun", bc.content_id, rr_scope, rr_scope_id,
                                       next_pos, eps[next_pos].episode_id);
                    }
                }
            } else if (!eps.empty()) {
                int pos      = readCursorPos("show_rerun", bc.content_id, rr_scope, rr_scope_id);
                int next_pos = (pos + 1) % static_cast<int>(eps.size());
                writeCursorPos("show_rerun", bc.content_id, rr_scope, rr_scope_id,
                               next_pos, eps[next_pos].episode_id);
            } else if (b.no_history_behavior == NoHistoryBehavior::FallbackAll) {
                auto all = getEpisodes(bc.content_id, bc.season_filter, bc.include_specials, bc.episode_order);
                if (!all.empty()) {
                    int pos      = readCursorPos("show_rerun", bc.content_id, rr_scope, rr_scope_id);
                    int next_pos = (pos + 1) % static_cast<int>(all.size());
                    writeCursorPos("show_rerun", bc.content_id, rr_scope, rr_scope_id,
                                   next_pos, all[next_pos].episode_id);
                }
            }

            int runs_rem = readRunsRemaining(b.block_id, channel_id);
            int consec   = readConsecutiveCount(b.block_id, channel_id) + 1;

            if (runs_rem <= 1) {
                int next_sel;
                if (b.no_history_behavior == NoHistoryBehavior::Exclude) {
                    int cnt = static_cast<int>(b.content.size());
                    std::vector<int> eligible;
                    int total_w = 0;
                    for (int i = 0; i < cnt; i++) {
                        const auto& cbc = b.content[i];
                        if (cbc.content_type == "show") {
                            auto ceps = (b.advancement == Advancement::RerunSmart)
                                ? getPlayedEpisodesWithCooldown(cbc.content_id, channel_id, cbc.season_filter, b.smart_pct, before_time, global, cbc.include_specials)
                                : getPlayedEpisodes(cbc.content_id, channel_id, cbc.season_filter, before_time, global, cbc.include_specials, cbc.episode_order);
                            if (ceps.empty()) continue;
                        }
                        eligible.push_back(i);
                        total_w += std::max(1, cbc.weight);
                    }
                    if (eligible.empty()) { writeRerunState(b.block_id, channel_id, 0, 0, 0); goto done; }
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
                {
                    bool same_show = (next_sel == rr);
                    bool limit_hit = (b.max_consecutive_episodes > 0 &&
                                      consec >= b.max_consecutive_episodes);
                    // Enforce the consecutive limit: re-select excluding the current show.
                    if (same_show && limit_hit && n > 1) {
                        int total_w = 0;
                        for (int i = 0; i < n; ++i)
                            if (i != rr) total_w += std::max(1, b.content[i].weight);
                        if (total_w > 0) {
                            std::uniform_int_distribution<int> dist(0, total_w - 1);
                            int r = dist(rng);
                            for (int i = 0; i < n; ++i) {
                                if (i == rr) continue;
                                r -= std::max(1, b.content[i].weight);
                                if (r < 0) { next_sel = i; break; }
                            }
                            same_show = false;
                        }
                    }
                    const auto& next_bc  = b.content[next_sel];
                    int         next_run = std::max(1, next_bc.run_count);
                    if (!same_show || limit_hit) {
                        if (b.advancement == Advancement::RerunSmart &&
                            b.no_history_behavior == NoHistoryBehavior::Normal) {
                            auto all_next = getEpisodes(next_bc.content_id, next_bc.season_filter,
                                                        next_bc.include_specials, next_bc.episode_order);
                            if (!all_next.empty()) {
                                auto next_eps = getPlayedEpisodesWithCooldown(
                                    next_bc.content_id, channel_id, next_bc.season_filter,
                                    b.smart_pct, before_time, global, next_bc.include_specials);
                                if (next_eps.empty()) {
                                    // No premiers: random entry into full catalog + snap (hook behavior).
                                    // Part 1 plays before Part 2 even when Part 2 is the random pick.
                                    std::uniform_int_distribution<int> dist(0, static_cast<int>(all_next.size()) - 1);
                                    int start = dist(rng);
                                    int snap  = b.snap_to_group_start
                                        ? snapToGroupStart(all_next[start].episode_id, all_next) : -1;
                                    int final_pos = (snap >= 0) ? snap : start;
                                    writeCursorPos("show", next_bc.content_id, scopeStr(b), scopeId(b, channel_id),
                                                   final_pos, all_next[final_pos].episode_id);
                                } else {
                                    // Has premiers: pick from played pool. Snap to Part 1 only if
                                    // Part 1 is also in the pool — snapToGroupStart searches next_eps
                                    // so it returns -1 when Part 1 hasn't aired yet.
                                    std::uniform_int_distribution<int> dist(0, static_cast<int>(next_eps.size()) - 1);
                                    int start = dist(rng);
                                    int snap_in_pool = b.snap_to_group_start
                                        ? snapToGroupStart(next_eps[start].episode_id, next_eps) : -1;
                                    const std::string& target_id = (snap_in_pool >= 0)
                                        ? next_eps[snap_in_pool].episode_id : next_eps[start].episode_id;
                                    int pos_in_all = 0;
                                    for (int i = 0; i < (int)all_next.size(); ++i)
                                        if (all_next[i].episode_id == target_id) { pos_in_all = i; break; }
                                    writeCursorPos("show", next_bc.content_id, scopeStr(b), scopeId(b, channel_id),
                                                   pos_in_all, all_next[pos_in_all].episode_id);
                                }
                            }
                        } else {
                            auto next_eps = (b.advancement == Advancement::RerunSmart)
                                ? getPlayedEpisodesWithCooldown(next_bc.content_id, channel_id, next_bc.season_filter, b.smart_pct, before_time, global, next_bc.include_specials)
                                : getPlayedEpisodes(next_bc.content_id, channel_id, next_bc.season_filter, before_time, global, next_bc.include_specials, next_bc.episode_order);
                            if (next_eps.empty() && b.no_history_behavior == NoHistoryBehavior::FallbackAll)
                                next_eps = getEpisodes(next_bc.content_id, next_bc.season_filter, next_bc.include_specials, next_bc.episode_order);
                            if (!next_eps.empty()) {
                                std::uniform_int_distribution<int> dist(0, static_cast<int>(next_eps.size()) - 1);
                                int start = dist(rng);
                                // Snap within next_eps: if pool-based, Part 1 must be in the pool
                                // (snapToGroupStart returns -1 when Part 1 isn't found in the list).
                                int snap = b.snap_to_group_start
                                    ? snapToGroupStart(next_eps[start].episode_id, next_eps) : -1;
                                int final_start = (snap >= 0) ? snap : start;
                                writeCursorPos("show_rerun", next_bc.content_id, rr_scope, rr_scope_id,
                                               final_start, next_eps[final_start].episode_id);
                            }
                        }
                        writeRerunState(b.block_id, channel_id, next_sel, next_run, 0);
                    } else {
                        writeRerunState(b.block_id, channel_id, next_sel, next_run, consec);
                    }
                }
            } else {
                writeRerunState(b.block_id, channel_id,
                                readBlockRR(b.block_id, channel_id), runs_rem - 1, consec);
            }
            done:;
        } else {
            // Advance the episode cursor within the current show.
            auto eps = getEpisodes(bc.content_id, bc.season_filter, bc.include_specials, bc.episode_order);
            if (!eps.empty()) {
                std::string scope    = scopeStr(b);
                std::string scope_id = scopeId(b, channel_id);
                int pos      = readCursorPos("show", bc.content_id, scope, scope_id);
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
            // Weighted show selection for the next slot.
            if (b.advancement == Advancement::Shuffle ||
                b.advancement == Advancement::SmartShuffle) {
                writeBlockRR(b.block_id, channel_id, selectWeighted(b, rng));
            } else {
                // Sequential: bc.weight = consecutive episodes per show before switching.
                int runs_rem = readRunsRemaining(b.block_id, channel_id);
                if (runs_rem == 0) runs_rem = std::max(1, bc.weight); // first play: initialize
                if (runs_rem <= 1) {
                    int next_rr   = (rr + 1) % n;
                    int next_runs = std::max(1, b.content[next_rr].weight);
                    writeRerunState(b.block_id, channel_id, next_rr, next_runs, 0);
                } else {
                    writeRerunState(b.block_id, channel_id, rr, runs_rem - 1, 0);
                }
            }
            return; // show selection handled; skip the global writeBlockRR below
        }
    } else if (bc.content_type == "playlist" || bc.content_type == "filler_list") {
        if (bc.content_type == "playlist" && getPlaylistMode(bc.content_id) == "show_collection") {
            auto shows = getPlaylistShows(bc.content_id);
            if (!shows.empty()) {
                int n_shows = static_cast<int>(shows.size());
                std::string scope    = scopeStr(b);
                std::string scope_id = scopeId(b, channel_id);
                int show_idx = readCursorPos("playlist", bc.content_id, scope, scope_id) % n_shows;
                const std::string& show_id = shows[show_idx];
                bool global = (b.cursor_scope == CursorScope::Global);

                if (isRerunMode(b.advancement)) {
                    // Advance the episode cursor within the current show
                    auto eps = (b.advancement == Advancement::RerunSmart)
                        ? getPlayedEpisodesWithCooldown(show_id, channel_id, std::nullopt, b.smart_pct, before_time, global)
                        : getPlayedEpisodes(show_id, channel_id, std::nullopt, before_time, global);
                    if (eps.empty() && b.no_history_behavior == NoHistoryBehavior::FallbackAll)
                        eps = getEpisodes(show_id, std::nullopt);
                    if (eps.empty() && b.no_history_behavior == NoHistoryBehavior::Normal) {
                        auto all = getEpisodes(show_id, std::nullopt);
                        if (!all.empty()) {
                            int pos      = readCursorPos("show", show_id, scope, scope_id);
                            int next_pos = (pos + 1) % static_cast<int>(all.size());
                            writeCursorPos("show", show_id, scope, scope_id,
                                           next_pos, all[next_pos].episode_id);
                        }
                    } else if (!eps.empty()) {
                        int pos      = readCursorPos("show_rerun", show_id, scope, scope_id);
                        int next_pos = (pos + 1) % static_cast<int>(eps.size());
                        writeCursorPos("show_rerun", show_id, scope, scope_id,
                                       next_pos, eps[next_pos].episode_id);
                    }

                    int runs_rem = readRunsRemaining(b.block_id, channel_id);
                    int consec   = readConsecutiveCount(b.block_id, channel_id) + 1;

                    if (runs_rem <= 1) {
                        // Always pick a different show when possible; uniform over the other n-1 shows.
                        int next_idx;
                        if (n_shows == 1) {
                            next_idx = 0;
                        } else {
                            std::uniform_int_distribution<int> dist(0, n_shows - 2);
                            int r = dist(rng);
                            next_idx = (r >= show_idx) ? r + 1 : r;
                        }

                        bool same_show = (next_idx == show_idx); // only true when n_shows == 1
                        bool limit_hit = (b.max_consecutive_episodes > 0 &&
                                          consec >= b.max_consecutive_episodes);

                        int next_run = std::max(1, bc.run_count);
                        if (!same_show || limit_hit) {
                            const std::string& next_show = shows[next_idx];
                            auto next_eps = (b.advancement == Advancement::RerunSmart)
                                ? getPlayedEpisodesWithCooldown(next_show, channel_id, std::nullopt, b.smart_pct, before_time, global)
                                : getPlayedEpisodes(next_show, channel_id, std::nullopt, before_time, global);
                            if (next_eps.empty() && b.no_history_behavior == NoHistoryBehavior::FallbackAll)
                                next_eps = getEpisodes(next_show, std::nullopt);
                            if (!next_eps.empty()) {
                                std::uniform_int_distribution<int> rdist(0, static_cast<int>(next_eps.size()) - 1);
                                int start = rdist(rng);
                                int snap  = b.snap_to_group_start
                                    ? snapToGroupStart(next_eps[start].episode_id, next_eps) : -1;
                                int final_start = (snap >= 0) ? snap : start;
                                writeCursorPos("show_rerun", next_show, scope, scope_id,
                                               final_start, next_eps[final_start].episode_id);
                            }
                            writeCursorPos("playlist", bc.content_id, scope, scope_id, next_idx);
                            writeRerunState(b.block_id, channel_id, rr, next_run, 0);
                        } else {
                            writeCursorPos("playlist", bc.content_id, scope, scope_id, next_idx);
                            writeRerunState(b.block_id, channel_id, rr, next_run, consec);
                        }
                    } else {
                        writeRerunState(b.block_id, channel_id, rr, runs_rem - 1, consec);
                    }
                } else {
                    // Sequential / Shuffle: advance episode within current show
                    auto pl_eps = getPlaylistShowEpisodes(bc.content_id, show_id);
                    if (!pl_eps.empty()) {
                        int pos      = readCursorPos("show", show_id, scope, scope_id);
                        int next_pos;
                        if (b.advancement == Advancement::Shuffle ||
                            b.advancement == Advancement::SmartShuffle) {
                            next_pos = pos + 1;
                        } else {
                            next_pos = (pos + 1) % static_cast<int>(pl_eps.size());
                        }
                        std::string ep_id = pl_eps[next_pos % static_cast<int>(pl_eps.size())].episode_id;
                        writeCursorPos("show", show_id, scope, scope_id, next_pos, ep_id);
                    }
                    // Show rotation: shuffle = random pick; sequential = round-robin with bc.weight
                    if (b.advancement == Advancement::Shuffle ||
                        b.advancement == Advancement::SmartShuffle) {
                        std::uniform_int_distribution<int> dist(0, n_shows - 1);
                        writeCursorPos("playlist", bc.content_id, scope, scope_id, dist(rng));
                    } else {
                        int runs_rem = readRunsRemaining(b.block_id, channel_id);
                        if (runs_rem == 0) runs_rem = std::max(1, bc.weight);
                        if (runs_rem <= 1) {
                            int next_idx  = (show_idx + 1) % n_shows;
                            int next_runs = std::max(1, bc.weight);
                            writeCursorPos("playlist", bc.content_id, scope, scope_id, next_idx);
                            writeRerunState(b.block_id, channel_id, rr, next_runs, 0);
                        } else {
                            writeRerunState(b.block_id, channel_id, rr, runs_rem - 1, 0);
                        }
                    }
                }
            }
        } else {
            // Sequential flat playlist / filler_list
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
    }
    if (b.advancement == Advancement::Shuffle || b.advancement == Advancement::SmartShuffle) {
        int next_rr = (b.advancement == Advancement::SmartShuffle && b.smart_pct > 0)
            ? selectWeightedSmartCooldown(b, channel_id, b.smart_pct, before_time, rng)
            : selectWeighted(b, rng);
        writeBlockRR(b.block_id, channel_id, next_rr);
    } else if (b.advancement == Advancement::RerunSmart && b.smart_pct > 0) {
        // For RerunSmart movie blocks: apply cooldown to prevent recently-played movies
        // from being re-selected. selectWeightedSmartCooldown falls back to selectWeighted
        // if the block is not all-movie content, so show-based RerunSmart (which returned
        // early above) is not affected here.
        writeBlockRR(b.block_id, channel_id,
            selectWeightedSmartCooldown(b, channel_id, b.smart_pct, before_time, rng));
    } else {
        writeBlockRR(b.block_id, channel_id, (rr + 1) % n);
    }
}

// ── Inter-filler clip picker ──────────────────────────────────────────────────

std::optional<ScheduledItem> RuleEngine::pickFillerSim(
    const std::string& channel_id,
    const Block& block,
    const std::vector<BlockFillerEntry>& pool,
    int64_t max_ms,
    SimState& state,
    Xoshiro256& rng,
    std::time_t before_time)
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

    // Load items from the selected filler source (filler_list, playlist, show, or movie).
    struct FI { std::string type, id; int64_t dur = 0; };
    std::vector<FI> items;
    if (fe.content_type == "filler_list") {
        SQLite::Statement q(db_.get(), R"(
            SELECT fi.item_type, fi.item_id,
                   COALESCE(e.duration_ms, m.duration_ms, 0)
            FROM filler_list_item fi
            LEFT JOIN episode e ON fi.item_type='episode' AND fi.item_id=e.episode_id
            LEFT JOIN movie   m ON fi.item_type='movie'   AND fi.item_id=m.movie_id
            WHERE fi.filler_list_id=? ORDER BY fi.position
        )");
        q.bind(1, fe.content_id);
        while (q.executeStep())
            items.push_back({q.getColumn(0).getString(), q.getColumn(1).getString(),
                             q.getColumn(2).getInt64()});
    } else if (fe.content_type == "playlist") {
        SQLite::Statement q(db_.get(), R"(
            SELECT pi.item_type, pi.item_id,
                   COALESCE(e.duration_ms, m.duration_ms, 0)
            FROM playlist_item pi
            LEFT JOIN episode e ON pi.item_type='episode' AND pi.item_id=e.episode_id
            LEFT JOIN movie   m ON pi.item_type='movie'   AND pi.item_id=m.movie_id
            WHERE pi.playlist_id=? ORDER BY pi.position
        )");
        q.bind(1, fe.content_id);
        while (q.executeStep())
            items.push_back({q.getColumn(0).getString(), q.getColumn(1).getString(),
                             q.getColumn(2).getInt64()});
    } else if (fe.content_type == "show") {
        std::string sql = "SELECT 'episode', episode_id, COALESCE(duration_ms, 0)"
                          " FROM episode WHERE show_id=?";
        if (fe.season_filter.has_value()) sql += " AND season=?";
        sql += " ORDER BY season, episode";
        SQLite::Statement q(db_.get(), sql);
        q.bind(1, fe.content_id);
        if (fe.season_filter.has_value()) q.bind(2, fe.season_filter.value());
        while (q.executeStep())
            items.push_back({q.getColumn(0).getString(), q.getColumn(1).getString(),
                             q.getColumn(2).getInt64()});
    } else if (fe.content_type == "movie") {
        SQLite::Statement q(db_.get(), "SELECT duration_ms FROM movie WHERE movie_id=?");
        q.bind(1, fe.content_id);
        if (q.executeStep())
            items.push_back({"movie", fe.content_id, q.getColumn(0).getInt64()});
    }
    if (items.empty()) return std::nullopt;

    // Determine item index within the list.
    int item_idx = 0;
    std::string pos_key = "fl_pos:" + fe.content_type + ":" + fe.content_id + ":" + block.block_id;

    if (fe.advancement == "sized") {
        // Build eligible set: clips that fit within max_ms.
        std::vector<int> eligible;
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
            if (items[i].dur > 0 && items[i].dur <= max_ms)
                eligible.push_back(i);
        if (eligible.empty()) return std::nullopt;

        // Prefer the least recently played eligible clip (never-played first).
        std::unordered_map<std::string, int64_t> last_played;
        {
            const char* sql = (before_time > 0)
                ? "SELECT item_id, MAX(aired_at) FROM play_history WHERE channel_id=? AND aired_at<=? GROUP BY item_id"
                : "SELECT item_id, MAX(aired_at) FROM play_history WHERE channel_id=? GROUP BY item_id";
            SQLite::Statement q(db_.get(), sql);
            q.bind(1, channel_id);
            if (before_time > 0) q.bind(2, static_cast<int64_t>(before_time));
            while (q.executeStep())
                last_played[q.getColumn(0).getString()] = q.getColumn(1).getInt64();
        }

        item_idx = eligible[0];
        int64_t oldest = last_played.count(items[eligible[0]].id)
                       ? last_played.at(items[eligible[0]].id) : -1;
        for (int i = 1; i < static_cast<int>(eligible.size()); ++i) {
            int idx = eligible[i];
            int64_t lat = last_played.count(items[idx].id)
                        ? last_played.at(items[idx].id) : -1;
            if (lat < oldest) { oldest = lat; item_idx = idx; }
        }
        // "sized" is stateless — no cursor to advance.
    } else if (fe.advancement == "shuffle") {
        if (!state.show_pos.count(pos_key))
            state.show_pos[pos_key] = static_cast<int>(rng() % static_cast<uint64_t>(items.size()));
        int& pos   = state.show_pos[pos_key];
        int  epoch = pos / static_cast<int>(items.size());
        int  idx   = pos % static_cast<int>(items.size());
        auto perm  = shufflePermutation(
            "fl:" + fe.content_type + ":" + fe.content_id + ":" + block.block_id + std::to_string(epoch),
            static_cast<int>(items.size()));
        item_idx = perm[idx];
        ++pos;
    } else { // sequential
        if (!state.show_pos.count(pos_key))
            state.show_pos[pos_key] = static_cast<int>(rng() % static_cast<uint64_t>(items.size()));
        int& pos = state.show_pos[pos_key];
        item_idx = pos % static_cast<int>(items.size());
        pos      = (pos + 1) % static_cast<int>(items.size());
    }

    const auto& fi = items[item_idx];

    ScheduledItem item;
    item.is_filler = true;
    if (fi.type == "episode") {
        auto ep = episodeById(fi.id);
        if (!ep) return std::nullopt;
        item = std::move(*ep);
        item.is_filler = true;
    } else {
        auto m = getMovie(fi.id);
        if (!m) return std::nullopt;
        item = movieItem(*m);
        item.is_filler = true;
    }
    item.channel_id = channel_id;
    item.block_id   = block.block_id;
    return item;
}

// ── Bumper / intro / outro / interstitial helpers ────────────────────────────

std::optional<ScheduledItem> RuleEngine::pickBumperItem(
    const std::string& channel_id,
    const std::string& content_type,
    const std::string& content_id,
    const std::string& scope_id)
{
    if (content_type.empty() || content_id.empty()) return std::nullopt;

    if (content_type == "episode") {
        return episodeById(content_id);
    }
    if (content_type == "show") {
        auto eps = getEpisodes(content_id, std::nullopt, true); // include specials
        if (eps.empty()) return std::nullopt;
        int pos = readCursorPos("show", content_id, "block", scope_id)
                  % static_cast<int>(eps.size());
        return itemFromShow(channel_id, "", eps, pos, showTitle(content_id));
    }
    if (content_type == "playlist") {
        auto items = loadListItems("playlist", content_id);
        if (items.empty()) return std::nullopt;
        int pos = readCursorPos("playlist", content_id, "block", scope_id)
                  % static_cast<int>(items.size());
        const auto& [ptype, pid] = items[pos];
        if (ptype == "episode") {
            return episodeById(pid);
        } else {
            auto m = getMovie(pid);
            if (!m) return std::nullopt;
            return movieItem(*m);
        }
    }
    return std::nullopt;
}

void RuleEngine::advanceBumperCursor(
    const std::string& content_type,
    const std::string& content_id,
    const std::string& scope_id)
{
    if (content_type == "episode") return; // single-episode bumper — no cursor

    if (content_type == "show") {
        auto eps = getEpisodes(content_id, std::nullopt, true);
        if (!eps.empty()) {
            int pos  = readCursorPos("show", content_id, "block", scope_id);
            int next = (pos + 1) % static_cast<int>(eps.size());
            writeCursorPos("show", content_id, "block", scope_id,
                           next, eps[next].episode_id);
        }
    } else if (content_type == "playlist") {
        SQLite::Statement q(db_.get(),
            "SELECT COUNT(*) FROM playlist_item WHERE playlist_id=?");
        q.bind(1, content_id);
        if (q.executeStep()) {
            int n = q.getColumn(0).getInt();
            if (n > 0) {
                int pos  = readCursorPos("playlist", content_id, "block", scope_id);
                writeCursorPos("playlist", content_id, "block", scope_id, (pos + 1) % n);
            }
        }
    }
}

bool RuleEngine::scheduleBumperItem(
    const std::string& channel_id,
    const std::string& block_id,
    const std::string& content_type,
    const std::string& content_id,
    const std::string& scope_id,
    std::vector<ScheduledItem>& result,
    std::time_t& t)
{
    auto item_opt = pickBumperItem(channel_id, content_type, content_id, scope_id);
    if (!item_opt || item_opt->duration_ms <= 0) return false;

    auto& item = *item_opt;
    item.channel_id          = channel_id;
    item.block_id            = block_id;
    item.wall_clock_start_ms = static_cast<int64_t>(t) * 1000;
    item.wall_clock_end_ms   = item.wall_clock_start_ms + item.duration_ms;
    item.cursor_json         = "{}";

    SQLite::Statement qph(db_.get(), R"(
        INSERT INTO play_history (item_type, item_id, channel_id, block_id, aired_at, is_scheduled)
        VALUES (?,?,?,?,?,1)
    )");
    qph.bind(1, item.item_type); qph.bind(2, item.item_id);
    qph.bind(3, channel_id);
    if (block_id.empty()) qph.bind(4); else qph.bind(4, block_id);
    qph.bind(5, static_cast<int64_t>(t));
    qph.exec();

    t += item.duration_ms / 1000;
    result.push_back(std::move(item));
    advanceBumperCursor(content_type, content_id, scope_id);
    return true;
}

// ── Forward projection ────────────────────────────────────────────────────────

std::vector<ScheduledItem> RuleEngine::project(const std::string& channel_id,
                                                std::time_t start, int horizon_hours,
                                                Xoshiro256& rng,
                                                std::map<std::time_t, std::string>* anchors_out) {
    std::vector<ScheduledItem> result;
    auto blocks = loadBlocks(channel_id);

    if (epgDebug())
        std::cout << "[epg] project() channel=" << channel_id
                  << " start=" << start << " hours=" << horizon_hours
                  << " blocks=" << blocks.size() << '\n';

    if (blocks.empty()) {
        std::cout << "[epg] WARNING: no blocks found for channel=" << channel_id << '\n';
        return result;
    }

    // Filler-only SimState (show_pos) — content cursors live in the DB.
    SimState state;

    // Channel-level filler entries: fallback when a block has no filler_entries.
    std::vector<BlockFillerEntry> channel_filler;
    {
        SQLite::Statement cfq(db_.get(), R"(
            SELECT content_type, content_id, advancement, weight, season_filter
            FROM channel_filler_entry WHERE channel_id = ? ORDER BY position
        )");
        cfq.bind(1, channel_id);
        while (cfq.executeStep()) {
            BlockFillerEntry fe;
            fe.content_type = cfq.getColumn(0).getString();
            fe.content_id   = cfq.getColumn(1).getString();
            fe.advancement  = cfq.getColumn(2).getString();
            fe.weight       = cfq.getColumn(3).getInt();
            if (!cfq.getColumn(4).isNull()) fe.season_filter = cfq.getColumn(4).getInt();
            channel_filler.push_back(std::move(fe));
        }
    }

    const std::string tz = channelTimezone(channel_id);

    // Load channel bumpers for "between" mode
    struct BetweenBumper { int id; std::string ct, cid; int every_n; };
    std::vector<BetweenBumper> between_bumpers;
    {
        SQLite::Statement bq(db_.get(),
            "SELECT id, content_type, content_id, every_n "
            "FROM channel_bumper WHERE channel_id = ? AND mode = 'between' "
            "ORDER BY position");
        bq.bind(1, channel_id);
        while (bq.executeStep())
            between_bumpers.push_back({bq.getColumn(0).getInt(),
                                       bq.getColumn(1).getString(),
                                       bq.getColumn(2).getString(),
                                       bq.getColumn(3).getInt()});
    }
    int channel_prog_count = 0; // non-filler content items scheduled so far

    std::time_t t   = start;
    std::time_t end = start + static_cast<std::time_t>(horizon_hours) * 3600;
    const int MAX_ITEMS = std::max(2000, horizon_hours * 300); // ~12s clip floor per hour
    std::string prev_block_id;
    std::unordered_map<std::string, int> prog_counts;  // programs scheduled per block occurrence
    std::unordered_set<std::string>      exhausted_blocks; // blocks that hit program_count this day
    int prev_day = -1;
    int dbg_null_streak = 0;
    std::unordered_set<std::string> seed_inited;    // blocks whose cursors have been seed-initialised
    std::unordered_set<std::string> intro_played;   // blocks whose intro fired this occurrence
    std::string last_show_id;                        // show_id of last scheduled content item
    std::unordered_map<std::string, int> transition_counts; // show transitions per block occurrence

    // Pre-populate exhausted_blocks from scheduled_program so that when the
    // ensureScheduled guard loop calls project() a second time (extending forward),
    // blocks that already hit program_count in the prior call are seen as exhausted.
    // Querying scheduled_program (not play_history) keeps exhaustion scoped to the
    // current scheduling session: a fresh rebuild from a past anchor starts with an
    // empty scheduled_program, so no blocks are pre-exhausted regardless of history.
    {
        auto tm_s  = toChannelTZ(start, tz);
        int  s_sec = tm_s.tm_hour * 3600 + tm_s.tm_min * 60 + tm_s.tm_sec;
        prev_day = tm_s.tm_year * 1000 + tm_s.tm_yday;
        std::time_t today_midnight = start - static_cast<std::time_t>(s_sec);
        // Compute tomorrow midnight safely to handle DST.
        std::time_t tomorrow_midnight;
        {
            std::time_t approx_tom = today_midnight + 90000; // 25h — always past midnight
            auto tm_tom  = toChannelTZ(approx_tom, tz);
            int  tom_sec = tm_tom.tm_hour * 3600 + tm_tom.tm_min * 60 + tm_tom.tm_sec;
            tomorrow_midnight = approx_tom - static_cast<std::time_t>(tom_sec);
        }
        for (const auto& b : blocks) {
            if (b.program_count <= 0) continue;
            SQLite::Statement qpc(db_.get(), R"(
                SELECT COUNT(*) FROM scheduled_program
                WHERE channel_id=? AND block_id=?
                  AND wall_clock_start >= ? AND wall_clock_start < ?
            )");
            qpc.bind(1, channel_id);
            qpc.bind(2, b.block_id);
            qpc.bind(3, static_cast<int64_t>(today_midnight));
            qpc.bind(4, static_cast<int64_t>(tomorrow_midnight));
            if (qpc.executeStep() && qpc.getColumn(0).getInt() >= b.program_count) {
                exhausted_blocks.insert(b.block_id);
                if (epgDebug())
                    std::cout << "[epg] pre-pop exhausted block=" << b.block_id.substr(0,8) << '\n';
            }
        }
    }

    // Next Monday midnight UTC after `start` — used to capture anchor snapshots.
    std::time_t anchor_next_monday = [&]() -> std::time_t {
        std::time_t d   = start / 86400;
        std::time_t dow = (d + 3) % 7;           // 0 = Mon
        return (d - dow + 7) * 86400;             // always the coming Monday, never start itself
    }();

    while (t < end && static_cast<int>(result.size()) < MAX_ITEMS) {
        // At each Monday midnight boundary, capture the RNG state + cursor snapshot
        // so future projections can restore to this exact point deterministically.
        if (anchors_out && t >= anchor_next_monday) {
            using json = nlohmann::json;
            json snap;
            snap["rng"] = rng.serialize();

            json cursors = json::array();
            {
                SQLite::Statement qc(db_.get(), R"(
                    SELECT content_type, content_id, cursor_scope, scope_id, position,
                           COALESCE(episode_id, '')
                    FROM media_cursor
                    WHERE (cursor_scope = 'channel' AND scope_id = ?)
                       OR (cursor_scope = 'block'
                           AND scope_id IN (SELECT block_id FROM block WHERE channel_id = ?))
                )");
                qc.bind(1, channel_id);
                qc.bind(2, channel_id);
                while (qc.executeStep()) {
                    cursors.push_back({
                        {"content_type",  qc.getColumn(0).getString()},
                        {"content_id",    qc.getColumn(1).getString()},
                        {"cursor_scope",  qc.getColumn(2).getString()},
                        {"scope_id",      qc.getColumn(3).getString()},
                        {"position",      qc.getColumn(4).getInt()},
                        {"episode_id",    qc.getColumn(5).getString()}
                    });
                }
            }
            snap["cursors"] = cursors;

            json block_states = json::array();
            {
                SQLite::Statement qbs(db_.get(), R"(
                    SELECT block_id, content_position, runs_remaining, consecutive_count
                    FROM block_state WHERE channel_id = ?
                )");
                qbs.bind(1, channel_id);
                while (qbs.executeStep()) {
                    block_states.push_back({
                        {"block_id",          qbs.getColumn(0).getString()},
                        {"content_position",  qbs.getColumn(1).getInt()},
                        {"runs_remaining",    qbs.getColumn(2).getInt()},
                        {"consecutive_count", qbs.getColumn(3).getInt()}
                    });
                }
            }
            snap["block_states"] = block_states;

            (*anchors_out)[anchor_next_monday] = snap.dump();
            anchor_next_monday += 7 * 86400;
        }

        // Clear exhausted_blocks when the calendar day rolls over (channel-local time).
        {
            auto tm_chk  = toChannelTZ(t, tz);
            int  cur_day = tm_chk.tm_year * 1000 + tm_chk.tm_yday;
            if (cur_day != prev_day) {
                if (epgDebug()) {
                    std::cout << "[epg] day rollover t=" << t
                              << " prev=" << prev_day << "→" << cur_day
                              << " exhausted_count=" << exhausted_blocks.size();
                    for (auto& id : exhausted_blocks) std::cout << ' ' << id.substr(0,8);
                    std::cout << '\n';
                }
                exhausted_blocks.clear();
                prev_day = cur_day;
            }
        }

        auto block_opt = resolveFromList(blocks, t, tz);
        // If the top-priority block is exhausted for this occurrence, re-resolve
        // against the remaining blocks so a lower-priority active block takes over
        // instead of falling through to the no-block (future-start-only) handler.
        if (block_opt && exhausted_blocks.count(block_opt->block_id)) {
            if (epgDebug())
                std::cout << "[epg] exhausted fallback t=" << t
                          << " block=" << block_opt->block_id.substr(0,8) << '\n';
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

            // Compute tomorrow's midnight in channel-local time. Adding 86400 is wrong
            // on DST spring-forward days (the day is only 23h), so we advance ~25h past
            // today's midnight and re-derive local midnight from the resulting timestamp.
            std::time_t tomorrow_midnight;
            {
                std::time_t approx_tomorrow = midnight + 90000; // 25h — always past midnight
                auto   tm_tom  = toChannelTZ(approx_tomorrow, tz);
                int    tom_sec = tm_tom.tm_hour * 3600 + tm_tom.tm_min * 60 + tm_tom.tm_sec;
                tomorrow_midnight = approx_tomorrow - static_cast<std::time_t>(tom_sec);
            }

            std::time_t jump = t + 1800; // fallback if no closer block found
            for (const auto& b : blocks) {
                auto try_day = [&](int bit, std::time_t base_midnight) {
                    if (!(b.day_mask & bit)) return;
                    std::time_t cand = base_midnight
                        + static_cast<std::time_t>(parseTimeMins(b.start_time)) * 60;
                    if (cand > t && cand < jump) jump = cand;
                };
                try_day(today_bit, midnight);
                try_day(tom_bit,   tomorrow_midnight);
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
            if (block.block_id != prev_block_id) {
                prog_counts[block.block_id] = 0;
                transition_counts[block.block_id] = 0;
                last_show_id.clear(); // don't carry show context across block boundaries
            }
            prev_block_id = block.block_id;
        }

        // Inject block intro on first entry to each block occurrence.
        if (!intro_played.count(block.block_id) && !block.intro_content_id.empty()) {
            intro_played.insert(block.block_id);
            scheduleBumperItem(channel_id, block.block_id,
                               block.intro_content_type, block.intro_content_id,
                               block.block_id + ":intro", result, t);
        }

        // On first entry to each block, apply seed-derived starting positions.
        // Premier blocks always start at S01E01 — never randomize their cursor.
        // Skip if block_state already exists: the channel has prior playback history
        // and re-seeding would clobber the current position on every cache clear.
        if (!seed_inited.count(block.block_id) &&
            block.block_type != BlockType::Premier) {
            seed_inited.insert(block.block_id);
            bool block_has_state = false;
            {
                SQLite::Statement qbs(db_.get(),
                    "SELECT 1 FROM block_state WHERE block_id=? AND channel_id=?");
                qbs.bind(1, block.block_id);
                qbs.bind(2, channel_id);
                block_has_state = qbs.executeStep();
            }
            if (!block_has_state) {
                if (isRerunMode(block.advancement)) {
                    bool global = (block.cursor_scope == CursorScope::Global);
                    int sel = selectWeighted(block, rng);
                    const auto& sel_bc = block.content[sel];
                    auto eps = (block.advancement == Advancement::RerunSmart)
                        ? getPlayedEpisodesWithCooldown(sel_bc.content_id, channel_id,
                                                        sel_bc.season_filter, block.smart_pct, t, global, sel_bc.include_specials)
                        : getPlayedEpisodes(sel_bc.content_id, channel_id, sel_bc.season_filter, t, global, sel_bc.include_specials, sel_bc.episode_order);
                    if (eps.empty() && block.no_history_behavior == NoHistoryBehavior::FallbackAll)
                        eps = getEpisodes(sel_bc.content_id, sel_bc.season_filter, sel_bc.include_specials, sel_bc.episode_order);
                    if (!eps.empty()) {
                        std::uniform_int_distribution<int> dist(0, static_cast<int>(eps.size()) - 1);
                        int start = dist(rng);
                        int snap  = block.snap_to_group_start
                            ? snapToGroupStart(eps[start].episode_id, eps) : -1;
                        int final_pos = (snap >= 0) ? snap : start;
                        writeCursorPos("show_rerun", sel_bc.content_id,
                                       scopeStr(block), scopeId(block, channel_id),
                                       final_pos, eps[final_pos].episode_id);
                    }
                    writeRerunState(block.block_id, channel_id, sel,
                                    std::max(1, block.content[sel].run_count), 0);
                } else {
                    for (const auto& bc : block.content) {
                        if (bc.content_type != "show") continue;
                        auto eps = getEpisodes(bc.content_id, bc.season_filter, bc.include_specials, bc.episode_order);
                        if (eps.empty()) continue;
                        int pos = static_cast<int>(rng() % static_cast<uint64_t>(eps.size()));
                        std::string scope    = scopeStr(block);
                        std::string scope_id = scopeId(block, channel_id);
                        writeCursorPos("show", bc.content_id, scope, scope_id,
                                       pos, eps[pos].episode_id);
                    }
                }
            }
        }

        auto item_opt = nextItem(channel_id, block, t);

        // Interstitial: inject between show transitions.
        if (item_opt && !block.interstitial_content_id.empty() &&
            block.interstitial_every_n > 0 &&
            !item_opt->show_id.empty() && !last_show_id.empty() &&
            item_opt->show_id != last_show_id)
        {
            int& tc = transition_counts[block.block_id];
            ++tc;
            if (tc % block.interstitial_every_n == 0) {
                scheduleBumperItem(channel_id, block.block_id,
                                   block.interstitial_content_type, block.interstitial_content_id,
                                   block.block_id + ":interstitial", result, t);
            }
        }

        bool is_fallback_filler = false;
        if (!item_opt) {
            const auto& filler_pool = block.filler_entries.empty() ? channel_filler
                                                                    : block.filler_entries;
            if (!filler_pool.empty())
                if (auto fi = pickFillerSim(channel_id, block, filler_pool, 0, state, rng, t)) {
                    item_opt           = std::move(fi);
                    is_fallback_filler = true;
                }
        }
        if (!item_opt) {
            ++dbg_null_streak;
            if (epgDebug() && (dbg_null_streak == 1 || dbg_null_streak % 100 == 0))
                std::cout << "[epg]   t=" << t << " nextItem null streak=" << dbg_null_streak
                          << " block=" << block.block_id << '\n';
            t += 60;
            continue;
        }
        if (dbg_null_streak > 0) {
            if (epgDebug() || dbg_null_streak >= 600)
                std::cout << "[epg] " << (dbg_null_streak >= 600 ? "WARNING: " : "")
                          << "nextItem returned null " << dbg_null_streak
                          << " consecutive times (" << (dbg_null_streak / 60)
                          << "m skipped) for channel=" << channel_id
                          << " block=" << block.block_id << '\n';
            dbg_null_streak = 0;
        }

        auto item = std::move(*item_opt);
        if (item.duration_ms <= 0) {
            std::cout << "[epg] WARNING: item duration_ms=0 id=" << item.item_id
                      << " type=" << item.item_type
                      << " block=" << block.block_id << " — skipping and advancing cursor\n";
            // Advance cursors so we don't spin forever on the same zero-duration item.
            if (!is_fallback_filler) advanceCursors(channel_id, block, t, rng);
            t += 60;
            continue;
        }

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

        // If this item would run past a higher-priority block's nominal start time,
        // snap t to that start and skip scheduling the item entirely. This keeps t
        // on the alignment grid so the late-start check passes next iteration.
        // (Capping to late_window_end left t mid-grid, causing alignment to push
        // effective_start past late_window_end, silently skipping the block.)
        {
            auto        tm_t2       = toChannelTZ(t, tz);
            int         cur_sec2    = tm_t2.tm_hour * 3600 + tm_t2.tm_min * 60 + tm_t2.tm_sec;
            std::time_t today_mid2  = t - static_cast<std::time_t>(cur_sec2);
            std::time_t item_end    = t + dur_ms / 1000;
            int         day_bit_now = dayBit(tm_t2);
            std::time_t tomorrow_mid2;
            {
                std::time_t approx_tom = today_mid2 + 90000;
                auto tm_tom = toChannelTZ(approx_tom, tz);
                int tom_sec = tm_tom.tm_hour * 3600 + tm_tom.tm_min * 60 + tm_tom.tm_sec;
                tomorrow_mid2 = approx_tom - static_cast<std::time_t>(tom_sec);
            }
            int         tom_bit    = 1 << ((tm_t2.tm_wday + 1) % 7);
            std::time_t preempt_to = 0;

            for (const auto& b : blocks) {
                if (b.block_id == block.block_id) break;
                if (b.late_start_mins <= 0) continue;

                auto snap_day = [&](int bit, std::time_t base_mid) {
                    if (!(b.day_mask & bit)) return;
                    std::time_t b_start = base_mid
                        + static_cast<std::time_t>(parseTimeMins(b.start_time)) * 60;
                    if (b_start <= t) return;
                    if (item_end > b_start)
                        if (preempt_to == 0 || b_start < preempt_to) preempt_to = b_start;
                };
                snap_day(day_bit_now, today_mid2);
                snap_day(tom_bit,     tomorrow_mid2);
            }
            if (preempt_to > 0) {
                t = preempt_to;
                continue;
            }
        }

        item.wall_clock_start_ms = static_cast<int64_t>(t) * 1000;
        item.wall_clock_end_ms   = item.wall_clock_start_ms + item.duration_ms;
        item.cursor_json         = "{}";

        const std::string ph_item_type = item.item_type;
        const std::string ph_item_id   = item.item_id;
        const std::time_t ph_aired_at  = t;

        // Track last show for interstitial detection.
        if (!is_fallback_filler) last_show_id = item.show_id;

        result.push_back(std::move(item));
        t += dur_ms / 1000;
        const std::time_t t_prog_end = t;

        // Write to play_history (is_scheduled=1) for recency tracking.
        // Only advance DB cursors for content items — fallback filler should not
        // move the content cursor, leaving it in place for when content becomes available.
        {
            SQLite::Statement qph(db_.get(), R"(
                INSERT INTO play_history
                    (item_type, item_id, channel_id, block_id, aired_at, is_scheduled)
                VALUES (?,?,?,?,?,1)
            )");
            qph.bind(1, ph_item_type);
            qph.bind(2, ph_item_id);
            qph.bind(3, channel_id);
            qph.bind(4, block.block_id);
            qph.bind(5, static_cast<int64_t>(ph_aired_at));
            qph.exec();
        }
        if (!is_fallback_filler) advanceCursors(channel_id, block, t, rng);

        // Channel between-bumpers: inject after every N non-filler content programs.
        if (!is_fallback_filler && !between_bumpers.empty()) {
            ++channel_prog_count;
            for (auto& bumper : between_bumpers) {
                if (bumper.every_n > 0 && channel_prog_count % bumper.every_n == 0) {
                    scheduleBumperItem(channel_id, block.block_id,
                                       bumper.ct, bumper.cid,
                                       channel_id + ":b" + std::to_string(bumper.id),
                                       result, t);
                    break; // one bumper per slot maximum
                }
            }
        }

        bool prog_limit_hit = !is_fallback_filler && (block.program_count > 0 &&
                               ++prog_counts[block.block_id] >= block.program_count);

        // ── Inter-filler injection ────────────────────────────────────────────
        // Only active for episode-scope aligned blocks: fills the gap between
        // the item that just ended and the next per-episode boundary. Clips can
        // nudge the next program into the early/late window rather than always
        // hard-snapping to the exact boundary; the alignment check below
        // accepts any position within those windows.
        // Individual clips are collected and then collapsed into a single merged
        // "filler" item so the preview shows one block instead of every clip.
        if (!is_fallback_filler && block.inter_filler && block.start_scope == "episode" && block.align_to_mins > 0) {
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
                    std::string last_cursor = simStateToJson(state).dump();

                    while (t < fill_target && static_cast<int>(result.size()) < MAX_ITEMS) {
                        int64_t remaining_ms = (fill_target - t) * 1000;
                        // A clip is only rejected if it would push the NEXT episode
                        // start past the late-start window entirely.  Using remaining_ms
                        // here (not late_boundary) was causing premature loop exit when
                        // sequential/shuffle returned a clip slightly longer than the gap.
                        int64_t max_clip_ms  = (late_boundary - t) * 1000;

                        auto fi = pickFillerSim(channel_id, block, pool, remaining_ms, state, rng, t);
                        if (!fi || fi->duration_ms <= 0) break;
                        if (fi->duration_ms > max_clip_ms) break;

                        // Record in play_history so recency queries in subsequent
                        // pickFillerSim() calls within this projection see this clip.
                        {
                            SQLite::Statement qph_f(db_.get(), R"(
                                INSERT INTO play_history
                                    (item_type, item_id, channel_id, block_id, aired_at, is_scheduled)
                                VALUES (?,?,?,?,?,1)
                            )");
                            qph_f.bind(1, fi->item_type);
                            qph_f.bind(2, fi->item_id);
                            qph_f.bind(3, channel_id);
                            qph_f.bind(4, block.block_id);
                            qph_f.bind(5, static_cast<int64_t>(t));
                            qph_f.exec();
                        }

                        fi->wall_clock_start_ms = static_cast<int64_t>(t) * 1000;
                        fi->wall_clock_end_ms   = fi->wall_clock_start_ms + fi->duration_ms;
                        last_cursor             = simStateToJson(state).dump();
                        fi->cursor_json         = last_cursor;
                        fi->channel_id          = channel_id;
                        fi->block_id            = block.block_id;
                        result.push_back(std::move(*fi));
                        t += fi->duration_ms / 1000;

                        // Stop once we've entered the early or late window.
                        std::time_t prev_b2 = (t / step) * step;
                        std::time_t next_b2 = prev_b2 + step;
                        bool now_early = (next_b2 - t) <= static_cast<std::time_t>(block.early_start_secs);
                        bool now_late  = (t - prev_b2) <= static_cast<std::time_t>(block.late_start_mins) * 60;
                        if (now_early || now_late) break;
                    }
                }
            }
        }

        // Episode-scope: apply alignment + early/late window after every content item.
        if (!is_fallback_filler && block.start_scope == "episode" && block.align_to_mins > 0) {
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
            // Outro: inject after the last content item of this block occurrence.
            if (!block.outro_content_id.empty()) {
                scheduleBumperItem(channel_id, block.block_id,
                                   block.outro_content_type, block.outro_content_id,
                                   block.block_id + ":outro", result, t);
            }
            if (block.align_to_mins > 0) {
                auto        tm_end   = toChannelTZ(t_prog_end, tz);
                int         cur_sec  = tm_end.tm_hour * 3600 + tm_end.tm_min * 60 + tm_end.tm_sec;
                std::time_t midnight = t_prog_end - static_cast<std::time_t>(cur_sec);
                std::time_t step     = static_cast<std::time_t>(block.align_to_mins) * 60;
                t = midnight + ((static_cast<std::time_t>(cur_sec) + step - 1) / step) * step;
            }
            if (epgDebug())
                std::cout << "[epg] exhausted t=" << t
                          << " block=" << block.block_id.substr(0,8)
                          << " prog_counts=" << prog_counts[block.block_id] << '\n';
            exhausted_blocks.insert(block.block_id);
            intro_played.erase(block.block_id);
            prev_block_id.clear();
        }
    }

    if (epgDebug() || result.empty())
        std::cout << "[epg] project() channel=" << channel_id
                  << " => " << result.size() << " items"
                  << (result.empty() ? " (EMPTY — no content scheduled)" : "") << '\n';

    return result;
}

// ── Playback completion ───────────────────────────────────────────────────────

void RuleEngine::markPlayed(const std::string& channel_id, const std::string& block_id,
                              const std::string& item_type, const std::string& item_id,
                              int64_t /*duration_ms*/) {
    SQLite::Transaction txn(db_.get());

    SQLite::Statement qh(db_.get(), R"(
        INSERT INTO play_history (item_type, item_id, channel_id, block_id, aired_at, is_scheduled)
        VALUES (?,?,?,?,?,0)
    )");
    qh.bind(1, item_type); qh.bind(2, item_id); qh.bind(3, channel_id);
    if (block_id.empty()) qh.bind(4); else qh.bind(4, block_id);
    qh.bind(5, static_cast<int64_t>(std::time(nullptr)));
    qh.exec();

    // In 'scheduled' mode project() already advanced cursors during EPG generation,
    // so advancing again here would double-advance and skip episodes on the next
    // ensureScheduled() pass. Only advance on confirmed play for 'on_play' channels.
    if (!block_id.empty() && channelAdvanceMode(channel_id) == "on_play") {
        auto blocks = loadBlocks(channel_id);
        for (const auto& b : blocks) {
            if (b.block_id != block_id) continue;
            // markPlayed is real-time; seed from channel+block hash for variety,
            // reproducibility is not required here.
            Xoshiro256 rng(std::hash<std::string>{}(channel_id + block_id)
                           ^ static_cast<uint64_t>(std::time(nullptr)));
            advanceCursors(channel_id, b, std::time(nullptr), rng);
            break;
        }
    }

    txn.commit();
}
