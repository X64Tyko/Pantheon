#include "RuleEngine.h"
#include "../db/BlockRepository.h"
#include "../db/ContentRepository.h"
#include "../db/CursorRepository.h"
#include "../db/Database.h"
#include "../db/ScheduleRepository.h"
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

static bool isRerunMode(const Block& block) {
    return block.play_style == PlayStyle::Rerun;
}

// ─────────────────────────────────────────────────────────────────────────────

RuleEngine::RuleEngine(Database& db) : db_(db), blocks_(db), content_(db) {}

// ── Static helpers ────────────────────────────────────────────────────────────

std::string RuleEngine::scopeStr(const Block& b) {
    if (isRerunMode(b) && b.cursor_scope == CursorScope::Block) {
        return "channel";
    }
    switch (b.cursor_scope) {
        case CursorScope::Global:  return "global";
        case CursorScope::Channel: return "channel";
        case CursorScope::Block:   return "block";
    }
    return "block";
}

std::string RuleEngine::scopeId(const Block& b, const std::string& channel_id) {
    if (isRerunMode(b) && b.cursor_scope == CursorScope::Block) {
        return channel_id;
    }
    switch (b.cursor_scope) {
        case CursorScope::Global:  return "";
        case CursorScope::Channel: return channel_id;
        case CursorScope::Block:   return b.block_id;
    }
    return b.block_id;
}

// ── DB loading (delegates to BlockRepository / ContentRepository) ─────────────

std::vector<Block> RuleEngine::loadBlocks(const std::string& channel_id) {
    return blocks_.loadBlocks(channel_id);
}

std::vector<Episode> RuleEngine::getEpisodes(const std::string& show_id,
                                              std::optional<int> season,
                                              bool include_specials,
                                              const std::string& episode_order) {
    return content_.getEpisodes(show_id, season, include_specials, episode_order);
}

std::vector<Episode> RuleEngine::getPlayedEpisodes(const std::string& show_id,
                                                     const std::string& channel_id,
                                                     std::optional<int> season,
                                                     std::time_t before_time,
                                                     bool global_scope,
                                                     bool include_specials,
                                                     const std::string& episode_order) {
    return content_.getPlayedEpisodes(show_id, channel_id, season, before_time,
                                      global_scope, include_specials, episode_order);
}

std::vector<Episode> RuleEngine::getPlayedEpisodesWithCooldown(const std::string& show_id,
                                                                  const std::string& channel_id,
                                                                  std::optional<int> season,
                                                                  int smart_pct,
                                                                  std::time_t before_time,
                                                                  bool global_scope,
                                                                  bool include_specials) {
    return content_.getPlayedEpisodesWithCooldown(show_id, channel_id, season, smart_pct,
                                                   before_time, global_scope, include_specials);
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
                                                        const std::vector<Episode>& eps) {
    if (eps.empty()) return {};

    auto ep_group = content_.getEpisodeGroupMap(eps[0].show_id);

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

int RuleEngine::selectWeightedExcluding(const Block& block, int exclude_idx, Xoshiro256& rng) {
    int total = 0;
    for (int i = 0; i < (int)block.content.size(); ++i)
        if (i != exclude_idx) total += std::max(1, block.content[i].weight);
    if (total <= 0) return selectWeighted(block, rng);
    std::uniform_int_distribution<int> dist(0, total - 1);
    int r = dist(rng);
    for (int i = 0; i < (int)block.content.size(); ++i) {
        if (i == exclude_idx) continue;
        r -= std::max(1, block.content[i].weight);
        if (r < 0) return i;
    }
    return selectWeighted(block, rng);
}

int RuleEngine::selectWeightedSmartCooldown(
    const Block& block, const std::string& channel_id,
    int smart_pct, std::time_t before_time,
    const std::vector<PlayRecord>& play_records,
    Xoshiro256& rng)
{
    int n = static_cast<int>(block.content.size());
    if (n <= 1 || smart_pct <= 0) return selectWeighted(block, rng);

    // Only apply movie-level cooldown when all content entries are movies.
    for (const auto& bc : block.content)
        if (bc.content_type != "movie") return selectWeighted(block, rng);

    int hot_count = std::max(0, n * smart_pct / 100);
    if (hot_count == 0) return selectWeighted(block, rng);

    auto hot_ids = content_.getHotMovieIds(channel_id, before_time, hot_count);

    // Also check in-pass records so items projected this run are considered hot.
    if (static_cast<int>(hot_ids.size()) < hot_count) {
        std::vector<std::pair<std::string, std::time_t>> pass_hot;
        for (const auto& pr : play_records) {
            if (pr.item_type == "movie" && pr.aired_at < before_time)
                pass_hot.push_back({pr.item_id, pr.aired_at});
        }
        std::sort(pass_hot.begin(), pass_hot.end(), [](auto& a, auto& b) {
            return a.second > b.second; // newest first
        });
        for (const auto& ph : pass_hot) {
            hot_ids.insert(ph.first);
            if (static_cast<int>(hot_ids.size()) >= hot_count) break;
        }
    }

    if (hot_ids.empty()) return selectWeighted(block, rng);

    int total = 0;
    for (const auto& bc : block.content)
        if (!hot_ids.contains(bc.content_id)) total += std::max(1, bc.weight);
    if (total <= 0) return selectWeighted(block, rng); // all hot — fall back

    std::uniform_int_distribution<int> dist(0, total - 1);
    int r = dist(rng);
    for (int i = 0; i < n; ++i) {
        if (hot_ids.contains(block.content[i].content_id)) continue;
        r -= std::max(1, block.content[i].weight);
        if (r <= 0) return i;
    }
    return selectWeighted(block, rng);
}

std::vector<Episode> RuleEngine::smartShufflePool(
    const std::vector<Episode>& all,
    const std::string& show_id,
    const std::string& channel_id,
    int smart_pct,
    std::time_t before_time,
    const std::vector<PlayRecord>& play_records)
{
    int n = static_cast<int>(all.size());
    int hot_count = std::max(0, n * smart_pct / 100);
    if (hot_count == 0 || all.empty()) return all;

    auto hot_ids = content_.getHotEpisodeIds(channel_id, before_time, show_id, hot_count);

    // Also check in-pass records so items projected this run are considered hot.
    if (static_cast<int>(hot_ids.size()) < hot_count) {
        std::vector<std::pair<std::string, std::time_t>> pass_hot;
        for (const auto& pr : play_records) {
            if (pr.item_type == "episode" && pr.show_id == show_id && pr.aired_at < before_time)
                pass_hot.push_back({pr.item_id, pr.aired_at});
        }
        std::sort(pass_hot.begin(), pass_hot.end(), [](auto& a, auto& b) {
            return a.second > b.second; // newest first
        });
        for (const auto& ph : pass_hot) {
            hot_ids.insert(ph.first);
            if (static_cast<int>(hot_ids.size()) >= hot_count) break;
        }
    }

    if (hot_ids.empty()) return all;

    std::vector<Episode> filtered;
    filtered.reserve(all.size());
    for (const auto& e : all)
        if (!hot_ids.contains(e.episode_id)) filtered.push_back(e);

    return filtered.empty() ? all : filtered;
}

int RuleEngine::snapToGroupStart(const std::string& episode_id,
                                  const std::vector<Episode>& eps) {
    auto part1 = content_.findGroupPart1(episode_id);
    if (!part1) return -1;
    for (int i = 0; i < static_cast<int>(eps.size()); ++i)
        if (eps[i].episode_id == *part1) return i;
    return -1;
}

std::optional<Movie> RuleEngine::getMovie(const std::string& movie_id) {
    return content_.getMovie(movie_id);
}

std::vector<std::pair<std::string, std::string>>
RuleEngine::loadListItems(const std::string& content_type, const std::string& content_id) {
    return content_.loadListItems(content_type, content_id);
}

std::string RuleEngine::getPlaylistMode(const std::string& playlist_id) {
    return content_.getPlaylistMode(playlist_id);
}

std::vector<std::string> RuleEngine::getPlaylistShows(const std::string& playlist_id) {
    return content_.getPlaylistShows(playlist_id);
}

std::vector<Episode> RuleEngine::getPlaylistShowEpisodes(const std::string& playlist_id,
                                                          const std::string& show_id) {
    return content_.getPlaylistShowEpisodes(playlist_id, show_id);
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
    return content_.showTitle(show_id);
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
    return blocks_.channelTimezone(channel_id);
}

std::string RuleEngine::channelAdvanceMode(const std::string& channel_id) {
    return blocks_.channelAdvanceMode(channel_id);
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
    // Peek-only: advance happens in a copy that is never persisted.
    CursorState state = CursorRepository(db_).load(channel_id);
    Xoshiro256 dummy_rng(0);
    int sel = pickNextContent(channel_id, block, before_time, state, {}, dummy_rng);
    if (sel < 0) return std::nullopt;
    return advanceAndGet(channel_id, block, sel, before_time, state, {}, dummy_rng);
}

// ── Episode pool and selection helpers ───────────────────────────────────────

// Returns the rerun pool for a show based on in-memory cursor state.
// - If a "show_rerun" cursor exists the show has completed its first run → full episode list.
// - If only a "show" cursor exists the show is mid first-run → episodes up to cursor position.
// - No cursor → DB play_history fallback (channel loaded before first projection).
// For Smart advancement the pool is then filtered by smart_pct recency cooldown.
std::vector<Episode> RuleEngine::getAvailableEpisodesForShow(
    const std::string& channel_id, const Block& block,
    const BlockContent& entry, std::time_t before_time,
    const CursorState& state, const std::vector<PlayRecord>& pass_records)
{
    const bool global   = (block.cursor_scope == CursorScope::Global);
    const auto scope    = scopeStr(block);
    const auto scope_id = scopeId(block, channel_id);

    std::vector<Episode> eps;

    if (state.hasCursor("show_rerun", entry.content_id, scope, scope_id)) {
        // Rerun phase: all episodes have been seen at least once.
        eps = getEpisodes(entry.content_id, entry.season_filter,
                          entry.include_specials, entry.episode_order);
    } else if (state.hasCursor("show", entry.content_id, scope, scope_id)) {
        // First-run in progress: episodes scheduled so far sit before the cursor.
        auto all = getEpisodes(entry.content_id, entry.season_filter,
                               entry.include_specials, entry.episode_order);
        int cp = state.getCursorPos("show", entry.content_id, scope, scope_id);
        for (int i = 0; i < cp && i < (int)all.size(); ++i)
            eps.push_back(all[i]);
    } else {
        // No in-memory cursor: fall back to DB play_history (pre-projection channel state).
        eps = (block.advancement == Advancement::Smart)
            ? getPlayedEpisodesWithCooldown(entry.content_id, channel_id, entry.season_filter,
                                            block.smart_pct, before_time, global, entry.include_specials)
            : getPlayedEpisodes(entry.content_id, channel_id, entry.season_filter,
                                before_time, global, entry.include_specials, entry.episode_order);
    }

    if (block.advancement == Advancement::Smart && block.smart_pct > 0 && !eps.empty())
        eps = smartShufflePool(eps, entry.content_id, channel_id,
                               block.smart_pct, before_time, pass_records);
    return eps;
}

// ── Rerun-block seed cursors ─────────────────────────────────────────────────

std::optional<RuleEngine::SeedCursor> RuleEngine::seedFromRealHistory(
    const std::vector<Episode>& played, const std::vector<Episode>& all,
    bool snap_to_group_start, Xoshiro256& rng)
{
    if (played.empty() || all.empty()) return std::nullopt;

    if (played.size() >= all.size()) {
        // Full catalog has aired at least once: seed straight into rerun mode.
        std::uniform_int_distribution<int> dist(0, static_cast<int>(all.size()) - 1);
        int sp = dist(rng);
        int sn = snap_to_group_start ? snapToGroupStart(all[sp].episode_id, all) : -1;
        int fp = sn >= 0 ? sn : sp;
        return SeedCursor{"show_rerun", fp, all[fp].episode_id};
    }
    // Partial history: still mid first-run. The "show" cursor is a prefix count —
    // getAvailableEpisodesForShow reads all[0..cp) back as the played pool — so cp
    // must be the real aired count, not a random guess.
    int cp = static_cast<int>(played.size());
    return SeedCursor{"show", cp, all[cp].episode_id};
}

std::optional<RuleEngine::SeedCursor> RuleEngine::getContentNormalHistory(
    const std::vector<Episode>& played, const std::vector<Episode>& all,
    bool snap_to_group_start, Xoshiro256& rng)
{
    if (auto seed = seedFromRealHistory(played, all, snap_to_group_start, rng)) return seed;
    if (all.empty()) return std::nullopt;
    std::uniform_int_distribution<int> dist(0, static_cast<int>(all.size()) - 1);
    int pos = dist(rng);
    return SeedCursor{"show", pos, all[pos].episode_id};
}

std::optional<RuleEngine::SeedCursor> RuleEngine::getContentFallbackHistory(
    const std::vector<Episode>& played, const std::vector<Episode>& all,
    bool snap_to_group_start, Xoshiro256& rng)
{
    if (auto seed = seedFromRealHistory(played, all, snap_to_group_start, rng)) return seed;
    if (all.empty()) return std::nullopt;
    std::uniform_int_distribution<int> dist(0, static_cast<int>(all.size()) - 1);
    int sp = dist(rng);
    int sn = snap_to_group_start ? snapToGroupStart(all[sp].episode_id, all) : -1;
    int fp = sn >= 0 ? sn : sp;
    return SeedCursor{"show_rerun", fp, all[fp].episode_id};
}

std::optional<RuleEngine::SeedCursor> RuleEngine::getContentSkipHistory(
    const std::vector<Episode>& played, const std::vector<Episode>& all,
    bool snap_to_group_start, Xoshiro256& rng)
{
    return seedFromRealHistory(played, all, snap_to_group_start, rng);
}

std::optional<RuleEngine::SeedCursor> RuleEngine::getContentExcludeHistory(
    const std::vector<Episode>& played, const std::vector<Episode>& all,
    bool snap_to_group_start, Xoshiro256& rng)
{
    return seedFromRealHistory(played, all, snap_to_group_start, rng);
}

// Selects and schedules the next episode for a rerun-mode show, advancing cursors.
// Normal mode: sequential first-run through all_eps, then rerun shuffle from pool.
// All other modes: rerun shuffle from pool (pool resolved by caller per no_history_behavior).
// Returns nullopt only when the pool is empty and we are past the first-run phase.
std::optional<ScheduledItem> RuleEngine::selectNextEpisode(
    const std::string& channel_id, const Block& block,
    const BlockContent& entry, const std::vector<Episode>& all_eps,
    const std::vector<Episode>& rerun_pool, CursorState& state)
{
    const auto scope    = scopeStr(block);
    const auto scope_id = scopeId(block, channel_id);

    if (block.no_history_behavior == NoHistoryBehavior::Normal &&
        !state.hasCursor("show_rerun", entry.content_id, scope, scope_id))
    {
        // First-run: advance sequentially through all_eps.
        int seq_pos  = state.getCursorPos("show", entry.content_id, scope, scope_id);
        auto item    = itemFromShow(channel_id, block.block_id, all_eps, seq_pos,
                                   showTitle(entry.content_id));
        int next_pos = seq_pos + 1;
        if (next_pos >= (int)all_eps.size()) {
            // First run complete: arm the rerun cursor so the next call enters rerun phase.
            state.setCursorPos("show_rerun", entry.content_id, scope, scope_id,
                               0, all_eps[0].episode_id);
            next_pos = 0;
        }
        state.setCursorPos("show", entry.content_id, scope, scope_id,
                           next_pos, all_eps[next_pos].episode_id);
        return item;
    }

    // Rerun shuffle phase (Normal post-first-run, FallbackAll, Exclude, Skip).
    if (rerun_pool.empty()) return std::nullopt;

    int  pos  = state.getCursorPos("show_rerun", entry.content_id, scope, scope_id);
    auto perm = groupedShufflePermutation(entry.content_id + block.block_id, rerun_pool);
    auto item = itemFromShow(channel_id, block.block_id, rerun_pool,
                             perm[pos % (int)perm.size()],
                             showTitle(entry.content_id));
    int next_pos = (pos + 1) % (int)rerun_pool.size();
    state.setCursorPos("show_rerun", entry.content_id, scope, scope_id,
                       next_pos, rerun_pool[next_pos].episode_id);
    return item;
}

// ── pickNextContent: block-level content selection ────────────────────────────
// Determines which content entry to play this call, updates block position state,
// and seeds show_rerun cursors on show transitions. Returns the content index,
// or -1 if Exclude mode exhausted all eligible entries.

int RuleEngine::pickNextContent(
    const std::string& channel_id, const Block& block,
    std::time_t before_time, CursorState& state,
    const std::vector<PlayRecord>& pass_records, Xoshiro256& rng)
{
    if (block.content.empty()) return 0;

    const int   n           = static_cast<int>(block.content.size());
    const int   content_pos = state.getContentPosition(block.block_id) % n;
    const auto& cur_entry   = block.content[content_pos];

    const std::string scope    = scopeStr(block);
    const std::string scope_id = scopeId(block, channel_id);

    const bool is_rerun = isRerunMode(block);
    const bool is_first = !state.hasBlockPosition(block.block_id);

    // Only rerun shows and non-rerun Sequential shows track run counts.
    const bool has_runs = is_rerun ||
        (cur_entry.content_type == "show" && block.advancement == Advancement::Sequential);

    const int  runs_left   = has_runs ? state.getRunsRemaining(block.block_id) : 0;
    const int  consecutive = state.getConsecutiveCount(block.block_id) + 1;
    const bool limit_hit   = block.max_consecutive_episodes > 0 &&
                             consecutive >= block.max_consecutive_episodes;
    const bool should_advance = !is_first && (!has_runs || runs_left == 0 || limit_hit);

    int sel = content_pos;

    if (should_advance) {
        if (is_rerun && block.no_history_behavior == NoHistoryBehavior::Exclude) {
            std::vector<int> eligible;
            int total_w = 0;
            for (int i = 0; i < n; ++i) {
                if (limit_hit && i == content_pos) continue;
                const auto& c = block.content[i];
                if (c.content_type == "show") {
                    auto pool = getAvailableEpisodesForShow(channel_id, block, c,
                                                            before_time, state, pass_records);
                    if (pool.empty()) continue;
                }
                eligible.push_back(i);
                total_w += std::max(1, c.weight);
            }
            if (eligible.empty()) {
                state.setBlockPosition(block.block_id, 0, 0, 0);
                return -1;
            }
            std::uniform_int_distribution<int> dist(0, total_w - 1);
            int r = dist(rng);
            sel = eligible.back();
            for (int idx : eligible) {
                r -= std::max(1, block.content[idx].weight);
                if (r < 0) { sel = idx; break; }
            }
        } else if (block.advancement == Advancement::Sequential) {
            sel = (content_pos + 1) % n;
        } else if (block.advancement == Advancement::Smart && block.smart_pct > 0) {
            sel = selectWeightedSmartCooldown(block, channel_id, block.smart_pct,
                                              before_time, pass_records, rng);
            if (limit_hit && sel == content_pos && n > 1)
                sel = selectWeightedExcluding(block, content_pos, rng);
        } else {
            sel = selectWeighted(block, rng);
            if (sel == content_pos && limit_hit && n > 1)
                sel = selectWeightedExcluding(block, content_pos, rng);
        }
    }

    const auto& sel_entry = block.content[sel];
    const bool  same      = (sel == content_pos) && !is_first;

    // Seed show_rerun cursor on show transition in rerun mode.
    if (is_rerun && !same && sel_entry.content_type == "show") {
        const bool in_first_run = block.no_history_behavior == NoHistoryBehavior::Normal &&
                                  !state.hasCursor("show_rerun", sel_entry.content_id, scope, scope_id);
        if (!in_first_run) {
            auto rerun_pool = getAvailableEpisodesForShow(channel_id, block, sel_entry,
                                                          before_time, state, pass_records);
            if (rerun_pool.empty() && block.no_history_behavior == NoHistoryBehavior::FallbackAll)
                rerun_pool = getEpisodes(sel_entry.content_id, sel_entry.season_filter,
                                         sel_entry.include_specials, sel_entry.episode_order);
            if (!rerun_pool.empty()) {
                std::uniform_int_distribution<int> rdist(0, (int)rerun_pool.size() - 1);
                int start       = rdist(rng);
                int snap        = block.snap_to_group_start
                    ? snapToGroupStart(rerun_pool[start].episode_id, rerun_pool) : -1;
                int final_start = snap >= 0 ? snap : start;
                state.setCursorPos("show_rerun", sel_entry.content_id, scope, scope_id,
                                   final_start, rerun_pool[final_start].episode_id);
            }
        }
    }

    // Update block state.
    const bool sel_has_runs = is_rerun ||
        (sel_entry.content_type == "show" && block.advancement == Advancement::Sequential);

    const int next_run = !sel_has_runs ? 0
        : is_rerun ? std::max(1, sel_entry.run_count) - 1
        : std::max(1, sel_entry.weight) - 1;

    if (!same) {
        state.setBlockPosition(block.block_id, sel, next_run, 0);
    } else {
        const int new_runs = (!is_first && !should_advance) ? runs_left - 1 : next_run;
        state.setBlockPosition(block.block_id, sel, new_runs, consecutive);
    }

    return sel;
}

// ── advanceAndGet: episode/item advancement for a pre-selected content entry ──
// content_idx is the index returned by pickNextContent. Handles only episode-level
// cursor advancement. Returns nullopt when no item is available.

std::optional<ScheduledItem> RuleEngine::advanceAndGet(
    const std::string& channel_id, const Block& block,
    int content_idx,
    std::time_t before_time, CursorState& state,
    const std::vector<PlayRecord>& pass_records,
    Xoshiro256& rng)
{
    if (block.content.empty()) return std::nullopt;

    const int   n           = static_cast<int>(block.content.size());
    const int   content_pos = content_idx % n;
    const auto& entry       = block.content[content_pos];

    std::optional<ScheduledItem> item;

    // ── Show ─────────────────────────────────────────────────────────────────────
    if (entry.content_type == "show") {
        const std::string scope    = scopeStr(block);
        const std::string scope_id = scopeId(block, channel_id);

        if (isRerunMode(block)) {
            // Episode pool — show was selected and transition-seeded by pickNextContent.
            const bool in_first_run = block.no_history_behavior == NoHistoryBehavior::Normal &&
                                      !state.hasCursor("show_rerun", entry.content_id, scope, scope_id);

            std::vector<Episode> rerun_pool;
            if (!in_first_run) {
                rerun_pool = getAvailableEpisodesForShow(channel_id, block, entry,
                                                         before_time, state, pass_records);
                if (rerun_pool.empty() && block.no_history_behavior == NoHistoryBehavior::FallbackAll)
                    rerun_pool = getEpisodes(entry.content_id, entry.season_filter,
                                             entry.include_specials, entry.episode_order);
            }

            auto all_eps = getEpisodes(entry.content_id, entry.season_filter,
                                       entry.include_specials, entry.episode_order);
            if (all_eps.empty()) return std::nullopt;

            return selectNextEpisode(channel_id, block, entry, all_eps, rerun_pool, state);

        } else {
            // ── Non-rerun show (Sequential / Shuffle / SmartShuffle) ───────────────
            auto all_eps = getEpisodes(entry.content_id, entry.season_filter,
                                       entry.include_specials, entry.episode_order);
            if (all_eps.empty()) return std::nullopt;

            int pos = state.getCursorPos("show", entry.content_id, scope, scope_id);
            int next_pos;

            // For sequential mode: reconcile the integer cursor position against the
            // stored episode_id. If the episode list changed since the cursor was last
            // saved (e.g. a sync added episodes), the integer position may point to the
            // wrong episode. Find the stored episode_id and resume from there.
            if (block.advancement == Advancement::Sequential) {
                const std::string stored_ep_id =
                    state.getCursorEpisodeId("show", entry.content_id, scope, scope_id);
                if (!stored_ep_id.empty()) {
                    int n = static_cast<int>(all_eps.size());
                    int safe_pos = pos % n;
                    if (all_eps[safe_pos].episode_id != stored_ep_id) {
                        // Episode list changed: search for the episode we meant to pick.
                        for (int i = 0; i < n; ++i) {
                            if (all_eps[i].episode_id == stored_ep_id) {
                                pos = i;
                                break;
                            }
                        }
                        // If not found (episode removed), pos stays at safe_pos (best effort).
                    }
                }
            }

            if (block.advancement == Advancement::Smart && block.smart_pct > 0) {
                auto eps  = smartShufflePool(all_eps, entry.content_id, channel_id,
                                             block.smart_pct, before_time, state.playRecords());
                auto perm = shufflePermutation(entry.content_id + block.block_id,
                                               static_cast<int>(eps.size()));
                item     = itemFromShow(channel_id, block.block_id, eps,
                                        perm[pos % static_cast<int>(perm.size())],
                                        showTitle(entry.content_id));
                next_pos = pos + 1;
            } else if (block.advancement == Advancement::Shuffle) {
                int epoch = pos / static_cast<int>(all_eps.size());
                int idx   = pos % static_cast<int>(all_eps.size());
                auto perm = shufflePermutation(
                    entry.content_id + block.block_id + std::to_string(epoch),
                    static_cast<int>(all_eps.size()));
                item     = itemFromShow(channel_id, block.block_id, all_eps, perm[idx],
                                        showTitle(entry.content_id));
                next_pos = pos + 1;
            } else {
                item     = itemFromShow(channel_id, block.block_id, all_eps, pos,
                                        showTitle(entry.content_id));
                next_pos = (pos + 1) % static_cast<int>(all_eps.size());
            }
            std::string ep_id = all_eps[next_pos % static_cast<int>(all_eps.size())].episode_id;
            state.setCursorPos("show", entry.content_id, scope, scope_id, next_pos, ep_id);
            return item; // Block position managed by pickNextContent

        }

    } else if (entry.content_type == "movie") {
        // ── Movie ─────────────────────────────────────────────────────────────────
        auto m = getMovie(entry.content_id);
        if (!m) return std::nullopt;
        auto mi = movieItem(*m);
        mi.channel_id = channel_id;
        mi.block_id   = block.block_id;
        item = std::move(mi);

    } else if (entry.content_type == "episode") {
        // ── Single episode ────────────────────────────────────────────────────────
        auto ei = episodeById(entry.content_id);
        if (!ei) return std::nullopt;
        ei->channel_id = channel_id;
        ei->block_id   = block.block_id;
        item = std::move(*ei);

    } else if (entry.content_type == "playlist" || entry.content_type == "filler_list") {
        if (entry.content_type == "playlist" && getPlaylistMode(entry.content_id) == "show_collection") {
            // ── show_collection ───────────────────────────────────────────────────
            auto shows = getPlaylistShows(entry.content_id);
            if (shows.empty()) return std::nullopt;
            int n_shows   = static_cast<int>(shows.size());
            const std::string scope    = scopeStr(block);
            const std::string scope_id = scopeId(block, channel_id);
            int show_idx  = state.getCursorPos("playlist", entry.content_id, scope, scope_id) % n_shows;
            const std::string& show_id = shows[show_idx];

            if (isRerunMode(block)) {
                const bool global = (block.cursor_scope == CursorScope::Global);
                auto eps = [&]() -> std::vector<Episode> {
                    if (state.hasCursor("show_rerun", show_id, scope, scope_id))
                        return getEpisodes(show_id, std::nullopt);
                    if (state.hasCursor("show", show_id, scope, scope_id)) {
                        auto all = getEpisodes(show_id, std::nullopt);
                        int cp = state.getCursorPos("show", show_id, scope, scope_id);
                        std::vector<Episode> partial;
                        for (int i = 0; i < cp && i < (int)all.size(); ++i)
                            partial.push_back(all[i]);
                        return partial;
                    }
                    return (block.advancement == Advancement::Smart)
                        ? getPlayedEpisodesWithCooldown(show_id, channel_id, std::nullopt,
                                                        block.smart_pct, before_time, global)
                        : getPlayedEpisodes(show_id, channel_id, std::nullopt, before_time, global);
                }();
                if (block.advancement == Advancement::Smart && block.smart_pct > 0 && !eps.empty())
                    eps = smartShufflePool(eps, show_id, channel_id,
                                           block.smart_pct, before_time, pass_records);
                if (eps.empty() && block.no_history_behavior == NoHistoryBehavior::FallbackAll)
                    eps = getEpisodes(show_id, std::nullopt);

                if (eps.empty() && block.no_history_behavior == NoHistoryBehavior::Normal) {
                    auto pl_eps = getPlaylistShowEpisodes(entry.content_id, show_id);
                    if (!pl_eps.empty()) {
                        int pos = state.getCursorPos("show", show_id, scope, scope_id);
                        item = itemFromShow(channel_id, block.block_id, pl_eps,
                                            pos, showTitle(show_id));
                        int next_pos = (pos + 1) % static_cast<int>(pl_eps.size());
                        state.setCursorPos("show", show_id, scope, scope_id,
                                           next_pos, pl_eps[next_pos].episode_id);
                    }
                } else if (!eps.empty()) {
                    int pos   = state.getCursorPos("show_rerun", show_id, scope, scope_id);
                    auto perm = groupedShufflePermutation(show_id + block.block_id, eps);
                    item = itemFromShow(channel_id, block.block_id, eps,
                                        perm[pos % static_cast<int>(perm.size())],
                                        showTitle(show_id));
                    int next_pos = (pos + 1) % static_cast<int>(eps.size());
                    state.setCursorPos("show_rerun", show_id, scope, scope_id,
                                       next_pos, eps[next_pos].episode_id);
                }

            } else {
                // Non-rerun show_collection
                auto pl_eps = getPlaylistShowEpisodes(entry.content_id, show_id);
                if (!pl_eps.empty()) {
                    int pos = state.getCursorPos("show", show_id, scope, scope_id);
                    int next_pos;
                    if (block.advancement == Advancement::Smart && block.smart_pct > 0) {
                        auto filt = smartShufflePool(pl_eps, show_id, channel_id,
                                                      block.smart_pct, before_time, state.playRecords());
                        auto perm = shufflePermutation(show_id + block.block_id,
                                                       static_cast<int>(filt.size()));
                        item     = itemFromShow(channel_id, block.block_id, filt,
                                                perm[pos % static_cast<int>(perm.size())],
                                                showTitle(show_id));
                        next_pos = pos + 1;
                    } else if (block.advancement == Advancement::Shuffle) {
                        int epoch = pos / static_cast<int>(pl_eps.size());
                        int idx   = pos % static_cast<int>(pl_eps.size());
                        auto perm = shufflePermutation(
                            show_id + block.block_id + std::to_string(epoch),
                            static_cast<int>(pl_eps.size()));
                        item     = itemFromShow(channel_id, block.block_id, pl_eps, perm[idx],
                                                showTitle(show_id));
                        next_pos = pos + 1;
                    } else {
                        item     = itemFromShow(channel_id, block.block_id, pl_eps, pos,
                                                showTitle(show_id));
                        next_pos = (pos + 1) % static_cast<int>(pl_eps.size());
                    }
                    std::string ep_id = pl_eps[next_pos % static_cast<int>(pl_eps.size())].episode_id;
                    state.setCursorPos("show", show_id, scope, scope_id, next_pos, ep_id);
                }
            }

            // Rotate within collection — always advance to next show.
            // Block-level content cycling is handled by pickNextContent.
            int next_idx;
            if (n_shows == 1) {
                next_idx = 0;
            } else if (block.advancement == Advancement::Shuffle ||
                       block.advancement == Advancement::Smart) {
                std::uniform_int_distribution<int> dist(0, n_shows - 1);
                next_idx = dist(rng);
            } else {
                next_idx = (show_idx + 1) % n_shows;
            }

            if (isRerunMode(block) && next_idx != show_idx) {
                // Seed the next show's rerun cursor at a random start position.
                const bool global = (block.cursor_scope == CursorScope::Global);
                const std::string& next_show = shows[next_idx];
                auto next_eps = [&]() -> std::vector<Episode> {
                    if (state.hasCursor("show_rerun", next_show, scope, scope_id))
                        return getEpisodes(next_show, std::nullopt);
                    if (state.hasCursor("show", next_show, scope, scope_id)) {
                        auto all = getEpisodes(next_show, std::nullopt);
                        int cp = state.getCursorPos("show", next_show, scope, scope_id);
                        std::vector<Episode> partial;
                        for (int i = 0; i < cp && i < (int)all.size(); ++i)
                            partial.push_back(all[i]);
                        return partial;
                    }
                    return (block.advancement == Advancement::Smart)
                        ? getPlayedEpisodesWithCooldown(next_show, channel_id, std::nullopt,
                                                        block.smart_pct, before_time, global)
                        : getPlayedEpisodes(next_show, channel_id, std::nullopt, before_time, global);
                }();
                if (block.advancement == Advancement::Smart && block.smart_pct > 0 && !next_eps.empty())
                    next_eps = smartShufflePool(next_eps, next_show, channel_id,
                                                block.smart_pct, before_time, pass_records);
                if (next_eps.empty() && block.no_history_behavior == NoHistoryBehavior::FallbackAll)
                    next_eps = getEpisodes(next_show, std::nullopt);
                if (!next_eps.empty()) {
                    std::uniform_int_distribution<int> rdist(0, static_cast<int>(next_eps.size()) - 1);
                    int start = rdist(rng);
                    int snap  = block.snap_to_group_start
                        ? snapToGroupStart(next_eps[start].episode_id, next_eps) : -1;
                    int final_start = (snap >= 0) ? snap : start;
                    state.setCursorPos("show_rerun", next_show, scope, scope_id,
                                       final_start, next_eps[final_start].episode_id);
                }
            }
            state.setCursorPos("playlist", entry.content_id, scope, scope_id, next_idx);

        } else {
            // ── Flat playlist / filler_list ───────────────────────────────────────
            auto list_items = loadListItems(entry.content_type, entry.content_id);
            if (list_items.empty()) return std::nullopt;

            const std::string scope    = scopeStr(block);
            const std::string scope_id = scopeId(block, channel_id);
            int pos = state.getCursorPos(entry.content_type, entry.content_id, scope, scope_id)
                      % static_cast<int>(list_items.size());
            const auto& [ptype, pid] = list_items[pos];

            if (ptype == "episode") {
                auto ei = episodeById(pid);
                if (!ei) return std::nullopt;
                ei->channel_id = channel_id;
                ei->block_id   = block.block_id;
                item = std::move(*ei);
            } else {
                auto mi = getMovie(pid);
                if (!mi) return std::nullopt;
                auto m = movieItem(*mi);
                m.channel_id = channel_id;
                m.block_id   = block.block_id;
                item = std::move(m);
            }
            state.setCursorPos(entry.content_type, entry.content_id, scope, scope_id,
                               (pos + 1) % static_cast<int>(list_items.size()));
        }
    }

    return item;
}

// ── Inter-filler clip picker ──────────────────────────────────────────────────

std::optional<ScheduledItem> RuleEngine::pickFillerSim(
    const std::string& channel_id,
    const Block& block,
    const std::vector<BlockFillerEntry>& pool,
    int64_t max_ms,
    CursorState& state,
    Xoshiro256& rng,
    const std::vector<PlayRecord>& pass_records,
    std::time_t before_time)
{
    if (pool.empty()) return std::nullopt;
    int pool_size = static_cast<int>(pool.size());

    // Select which filler entry (filler list) to pull from.
    int entry_idx = 0;
    if (block.filler_selection == "round_robin") {
        std::string rr_key = "fl_rr:" + block.block_id;
        int& rr   = state.fillerPos(rr_key);
        entry_idx = rr % pool_size;
        rr        = (rr + 1) % pool_size;
    } else {
        if (block.filler_selection == "weighted") {
            int total = 0;
            for (const auto& e : pool) total += std::max(1, e.weight);
            std::uniform_int_distribution<int> dist(0, total - 1);
            int r = dist(rng);
            for (int i = 0; i < pool_size; ++i) {
                r -= std::max(1, pool[i].weight);
                if (r < 0) { entry_idx = i; break; }
            }
        } else { // random
            std::uniform_int_distribution<int> dist(0, pool_size - 1);
            entry_idx = dist(rng);
        }
    }

    // Load items from the selected filler source. If empty, try remaining pool entries in
    // order — a permanently empty source (unlinked list, no media) should not block filler
    // from other entries. The round-robin state is not rewound for the failed entry.
    struct FI { std::string type, id; int64_t dur = 0; };
    std::vector<FI> items;
    {
        int tried = 0;
        while (tried < pool_size) {
            for (const auto& fi : content_.loadFillerItems(pool[entry_idx].content_type,
                                                            pool[entry_idx].content_id,
                                                            pool[entry_idx].season_filter))
                items.push_back({fi.item_type, fi.item_id, fi.duration_ms});
            if (!items.empty()) break;
            entry_idx = (entry_idx + 1) % pool_size;
            ++tried;
        }
    }
    if (items.empty()) return std::nullopt;
    const auto& fe = pool[entry_idx];

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
        auto last_played = content_.getLastPlayedMap(channel_id, before_time);

        item_idx = eligible[0];
        int64_t oldest = last_played.contains(items[eligible[0]].id)
                       ? last_played.at(items[eligible[0]].id) : -1;
        for (int i = 1; i < static_cast<int>(eligible.size()); ++i) {
            int idx = eligible[i];
            int64_t lat = last_played.contains(items[idx].id)
                        ? last_played.at(items[idx].id) : -1;
            if (lat < oldest) { oldest = lat; item_idx = idx; }
        }
        // "sized" is stateless — no cursor to advance.
    } else if (fe.advancement == "shuffle") {
        if (!state.hasFillerPos(pos_key))
            state.fillerPos(pos_key) = static_cast<int>(rng() % static_cast<uint64_t>(items.size()));
        int& pos   = state.fillerPos(pos_key);
        int  epoch = pos / static_cast<int>(items.size());
        int  idx   = pos % static_cast<int>(items.size());
        auto perm  = shufflePermutation(
            "fl:" + fe.content_type + ":" + fe.content_id + ":" + block.block_id + std::to_string(epoch),
            static_cast<int>(items.size()));
        item_idx = perm[idx];
        ++pos;
    } else { // sequential
        if (!state.hasFillerPos(pos_key))
            state.fillerPos(pos_key) = static_cast<int>(rng() % static_cast<uint64_t>(items.size()));
        int& pos = state.fillerPos(pos_key);
        item_idx = pos % static_cast<int>(items.size());
        pos      = (pos + 1) % static_cast<int>(items.size());
    }

    // smart_pct cooldown for shuffle/sequential: when the cursor-selected item is in the
    // hottest smart_pct% of the pool by recency, scan forward for a cooler alternative.
    // Sized advancement already applies LRU directly; this covers the other two modes.
    if (fe.advancement != "sized" && block.smart_pct > 0 && before_time > 0) {
        auto lp    = content_.getLastPlayedMap(channel_id, before_time);
        
        // Augment last-played map with in-pass records.
        for (const auto& pr : pass_records) {
            if (pr.channel_id == channel_id && pr.aired_at < before_time) {
                auto it = lp.find(pr.item_id);
                if (it == lp.end() || pr.aired_at > it->second)
                    lp[pr.item_id] = pr.aired_at;
            }
        }

        int  hot_n = std::max(0, static_cast<int>(items.size()) * block.smart_pct / 100);
        if (hot_n > 0 && !lp.empty()) {
            std::vector<int64_t> ts;
            ts.reserve(items.size());
            for (const auto& fitem : items) {
                auto it = lp.find(fitem.id);
                ts.push_back(it != lp.end() ? it->second : -1);
            }
            auto sorted_ts = ts;
            std::sort(sorted_ts.rbegin(), sorted_ts.rend());
            int64_t thresh = sorted_ts[hot_n - 1];
            if (thresh >= 0 && ts[item_idx] >= thresh) {
                int m = static_cast<int>(items.size());
                for (int d = 1; d < m; ++d) {
                    int c = (item_idx + d) % m;
                    if (ts[c] < thresh || ts[c] < 0) { item_idx = c; break; }
                }
            }
        }
    }

    const auto& fi = items[item_idx];
    auto item_opt  = pickFromSource(channel_id, fi.type, fi.id, 0);
    if (!item_opt) return std::nullopt;
    auto item       = std::move(*item_opt);
    item.is_filler  = true;
    item.channel_id = channel_id;
    item.block_id   = block.block_id;
    return item;
}

// ── Bumper / intro / outro / interstitial helpers ────────────────────────────

std::optional<ScheduledItem> RuleEngine::pickFromSource(
    const std::string& channel_id,
    const std::string& content_type,
    const std::string& content_id,
    int position)
{
    if (content_type == "episode")
        return episodeById(content_id);
    if (content_type == "movie") {
        auto m = getMovie(content_id);
        if (!m) return std::nullopt;
        return movieItem(*m);
    }
    if (content_type == "show") {
        auto eps = getEpisodes(content_id, std::nullopt, true);
        if (eps.empty()) return std::nullopt;
        return itemFromShow(channel_id, "", eps,
                            position % static_cast<int>(eps.size()),
                            showTitle(content_id));
    }
    if (content_type == "playlist") {
        auto list = loadListItems("playlist", content_id);
        if (list.empty()) return std::nullopt;
        const auto& [ptype, pid] = list[position % static_cast<int>(list.size())];
        if (ptype == "episode") return episodeById(pid);
        auto m = getMovie(pid);
        if (!m) return std::nullopt;
        return movieItem(*m);
    }
    return std::nullopt;
}

std::optional<ScheduledItem> RuleEngine::pickBumperItem(
    const std::string& channel_id,
    const std::string& content_type,
    const std::string& content_id,
    const std::string& scope_id,
    CursorState& state)
{
    if (content_type.empty() || content_id.empty()) return std::nullopt;

    int pos = 0;
    if (content_type == "show")
        pos = state.getCursorPos("show", content_id, "block", scope_id);
    else if (content_type == "playlist")
        pos = state.getCursorPos("playlist", content_id, "block", scope_id);

    return pickFromSource(channel_id, content_type, content_id, pos);
}

void RuleEngine::advanceBumperCursor(
    const std::string& content_type,
    const std::string& content_id,
    const std::string& scope_id,
    CursorState& state)
{
    if (content_type == "episode") return; // single-episode bumper — no cursor

    if (content_type == "show") {
        auto eps = getEpisodes(content_id, std::nullopt, true);
        if (!eps.empty()) {
            int pos  = state.getCursorPos("show", content_id, "block", scope_id);
            int next = (pos + 1) % static_cast<int>(eps.size());
            state.setCursorPos("show", content_id, "block", scope_id,
                           next, eps[next].episode_id);
        }
    } else if (content_type == "playlist") {
        int n = content_.getPlaylistItemCount(content_id);
        if (n > 0) {
            int pos = state.getCursorPos("playlist", content_id, "block", scope_id);
            state.setCursorPos("playlist", content_id, "block", scope_id, (pos + 1) % n);
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
    std::time_t& t,
    CursorState& state)
{
    auto item_opt = pickBumperItem(channel_id, content_type, content_id, scope_id, state);
    if (!item_opt || item_opt->duration_ms <= 0) return false;

    auto& item = *item_opt;
    item.channel_id          = channel_id;
    item.block_id            = block_id;
    item.wall_clock_start_ms = static_cast<int64_t>(t) * 1000;
    item.wall_clock_end_ms   = item.wall_clock_start_ms + item.duration_ms;
    item.cursor_json         = "{}";

    t += item.duration_ms / 1000;
    result.push_back(std::move(item));
    advanceBumperCursor(content_type, content_id, scope_id, state);
    return true;
}

// ── Three-layer scheduling core ──────────────────────────────────────────────
//
//  scheduleBlock  — item loop for one block occurrence in [pass.t, window_end).
//                   Returns true if program_count exhausted, false otherwise.
//
//  projectDay     — dispatch loop for one calendar day. Owns a local exhausted
//                   set (reset each day). Calls scheduleBlock per occurrence.
//
//  project        — outer loop: one TZ call per day, calls projectDay per day.

bool RuleEngine::scheduleBlock(
    const ProjectContext& ctx,
    const Block& block,
    std::time_t window_end,
    int window_late_start_mins,
    bool first_entry,
    std::time_t day_start,
    ProjectPassState& pass)
{
    if (block.block_type == BlockType::Timeslot)
        return scheduleTimeslotBlock(ctx, block, window_end, window_late_start_mins, day_start, pass);

    if (first_entry && !block.intro_content_id.empty())
        scheduleBumperItem(ctx.channel_id, block.block_id,
                           block.intro_content_type, block.intro_content_id,
                           block.block_id + ":intro", ctx.result, pass.t, ctx.state);

    // Soft end via block.end_time; cross-midnight blocks have end_secs < start_secs.
    std::time_t end_time_ts = std::numeric_limits<std::time_t>::max();
    if (block.end_time.has_value()) {
        int end_secs   = parseTimeMins(*block.end_time) * 60;
        int start_secs = parseTimeMins(block.start_time) * 60;
        std::time_t base = (end_secs < start_secs) ? day_start + 86400 : day_start;
        end_time_ts = base + static_cast<std::time_t>(end_secs);
    }
    std::time_t loop_end = std::min(window_end, end_time_ts);

    int prog_count      = 0;
    int dbg_null_streak = 0;

    while (pass.t < loop_end) {
        if (ctx.anchors_out && pass.t >= pass.anchor_next_monday) {
            using json = nlohmann::json;
            auto csnap = json::parse(ctx.state.serializeCursors());
            json snap{
                {"rng",          ctx.rng.serialize()},
                {"cursors",      csnap["cursors"]},
                {"block_states", csnap["block_states"]}
            };
            (*ctx.anchors_out)[pass.anchor_next_monday] = snap.dump();
            pass.anchor_next_monday += 7 * 86400;
        }

        int sel = pickNextContent(ctx.channel_id, block, pass.t, ctx.state, pass.play_records, ctx.rng);

        // Determine whether the selected content entry is a show type. Only show-type
        // entries should update last_show_id — filler_list, playlist, movie, and episode
        // entries must not, otherwise an interstitial bumper fires between consecutive
        // episodes of the same show (e.g. two Owl House episodes separated by inter-filler)
        // because a filler_list item's show_id pollutes the tracker, making the next
        // same-show episode look like a show transition.
        const bool content_entry_is_show = sel >= 0 && !block.content.empty() &&
            block.content[sel % static_cast<int>(block.content.size())].content_type == "show";

        CursorState snap = ctx.state;
        auto item_opt = (sel >= 0)
            ? advanceAndGet(ctx.channel_id, block, sel, pass.t, ctx.state, pass.play_records, ctx.rng)
            : std::nullopt;

        if (item_opt && !block.interstitial_content_id.empty() &&
            block.interstitial_every_n > 0 &&
            !item_opt->show_id.empty() && !pass.last_show_id.empty() &&
            item_opt->show_id != pass.last_show_id)
        {
            int& tc = pass.transition_counts[block.block_id];
            if (++tc % block.interstitial_every_n == 0)
                scheduleBumperItem(ctx.channel_id, block.block_id,
                                   block.interstitial_content_type, block.interstitial_content_id,
                                   block.block_id + ":interstitial", ctx.result, pass.t, ctx.state);
        }

        bool is_fallback_filler = false;
        if (!item_opt) {
            const auto& pool = block.filler_entries.empty() ? ctx.channel_filler
                                                            : block.filler_entries;
            if (!pool.empty())
                if (auto fi = pickFillerSim(ctx.channel_id, block, pool, 0, ctx.state, ctx.rng, pass.play_records, pass.t)) {
                    item_opt           = std::move(fi);
                    is_fallback_filler = true;
                }
        }
        if (!item_opt) {
            ++dbg_null_streak;
            if (epgDebug() && (dbg_null_streak == 1 || dbg_null_streak % 100 == 0))
                std::cout << "[epg]   t=" << pass.t << " nextItem null streak=" << dbg_null_streak
                          << " block=" << block.block_id << '\n';
            pass.t += 60;
            continue;
        }
        if (dbg_null_streak > 0) {
            if (epgDebug() || dbg_null_streak >= 600)
                std::cout << "[epg] " << (dbg_null_streak >= 600 ? "WARNING: " : "")
                          << "nextItem returned null " << dbg_null_streak
                          << " consecutive times (" << (dbg_null_streak / 60)
                          << "m skipped) for channel=" << ctx.channel_id
                          << " block=" << block.block_id << '\n';
            dbg_null_streak = 0;
        }

        auto item = std::move(*item_opt);
        if (item.duration_ms <= 0) {
            std::cout << "[epg] WARNING: item duration_ms=0 id=" << item.item_id
                      << " type=" << item.item_type
                      << " block=" << block.block_id << " — skipping\n";
            pass.t += 60;
            continue;
        }

        int64_t dur_ms = item.duration_ms;

        if (block.end_time.has_value()) {
            int64_t rem = (end_time_ts - pass.t) * 1000;
            if (rem <= 0) { pass.t += 60; continue; }
            dur_ms = std::min(dur_ms, rem);
        }

        // Preemption: if the episode can't finish within the preempting block's
        // late-start grace window, roll back and exit cleanly at window_end.
        // Episodes that fit within the grace are allowed to run over; filler and
        // the loop exit condition still use the strict window_end.
        const std::time_t window_soft = window_end
            + static_cast<std::time_t>(window_late_start_mins) * 60;
        if (pass.t + dur_ms / 1000 > window_soft) {
            ctx.state = snap;
            pass.t    = window_end;
            return false;
        }

        item.wall_clock_start_ms = static_cast<int64_t>(pass.t) * 1000;
        item.wall_clock_end_ms   = item.wall_clock_start_ms + dur_ms;
        item.cursor_json         = "{}";

        const std::string ph_type = item.item_type;
        const std::string ph_id   = item.item_id;
        const std::time_t ph_at   = pass.t;
        const std::string ph_show = item.show_id;

        if (!is_fallback_filler && content_entry_is_show) pass.last_show_id = item.show_id;
        ctx.result.push_back(std::move(item));
        pass.t += dur_ms / 1000;
        const std::time_t t_prog_end = pass.t;

        if (!is_fallback_filler) {
            pass.play_records.push_back({ctx.channel_id, ph_type, ph_id, ph_show, block.block_id, ph_at});
            ctx.state.addPlayRecord(ctx.channel_id, ph_type, ph_id, ph_show, block.block_id, ph_at);

            // Cull pass.play_records based on rerun_min_time_mins.
            int effective_min = block.rerun_min_time_mins > 0 ? block.rerun_min_time_mins : ctx.rerun_min_time_mins;
            if (effective_min > 0) {
                std::time_t threshold = pass.t - static_cast<std::time_t>(effective_min) * 60;
                pass.play_records.erase(
                    std::remove_if(pass.play_records.begin(), pass.play_records.end(),
                                   [&](const PlayRecord& pr) { return pr.aired_at < threshold; }),
                    pass.play_records.end());
            }
        }

        if (!is_fallback_filler && !ctx.between_bumpers.empty()) {
            ++pass.channel_prog_count;
            for (const auto& bumper : ctx.between_bumpers) {
                if (bumper.every_n > 0 && pass.channel_prog_count % bumper.every_n == 0) {
                    scheduleBumperItem(ctx.channel_id, block.block_id, bumper.ct, bumper.cid,
                                       ctx.channel_id + ":b" + std::to_string(bumper.id),
                                       ctx.result, pass.t, ctx.state);
                    break;
                }
            }
        }

        bool prog_limit_hit = !is_fallback_filler && block.program_count > 0 &&
                               ++prog_count >= block.program_count;

        if (!is_fallback_filler && block.inter_filler &&
            block.start_scope == "episode" && block.align_to_mins > 0) {
            const auto& pool = block.filler_entries.empty() ? ctx.channel_filler
                                                            : block.filler_entries;
            if (!pool.empty()) {
                std::time_t step   = static_cast<std::time_t>(block.align_to_mins) * 60;
                std::time_t prev_b = (pass.t / step) * step;
                std::time_t next_b = prev_b + step;
                bool in_early = (next_b - pass.t) <= static_cast<std::time_t>(block.early_start_secs);
                bool in_late  = (pass.t - prev_b)  <= static_cast<std::time_t>(block.late_start_mins) * 60;
                if (!in_early && !in_late) {
                    const std::time_t fill_target   = next_b;
                    const std::time_t late_boundary = next_b
                        + static_cast<std::time_t>(block.late_start_mins) * 60;
                    while (pass.t < fill_target) {
                        int64_t rem_ms = (fill_target - pass.t) * 1000;
                        int64_t max_ms = (late_boundary - pass.t) * 1000;
                        auto fi = pickFillerSim(ctx.channel_id, block, pool, rem_ms, ctx.state, ctx.rng, pass.play_records, pass.t);
                        if (!fi || fi->duration_ms <= 0 || fi->duration_ms > max_ms) break;
                        fi->wall_clock_start_ms = static_cast<int64_t>(pass.t) * 1000;
                        fi->wall_clock_end_ms   = fi->wall_clock_start_ms + fi->duration_ms;
                        fi->cursor_json         = "{}";
                        fi->channel_id          = ctx.channel_id;
                        fi->block_id            = block.block_id;
                        const int64_t fi_dur    = fi->duration_ms;
                        ctx.result.push_back(std::move(*fi));
                        pass.t += fi_dur / 1000;
                        std::time_t pb2 = (pass.t / step) * step, nb2 = pb2 + step;
                        if ((nb2 - pass.t) <= static_cast<std::time_t>(block.early_start_secs) ||
                            (pass.t - pb2)  <= static_cast<std::time_t>(block.late_start_mins) * 60)
                            break;
                    }
                }
            }
        }

        if (!is_fallback_filler && block.start_scope == "episode" && block.align_to_mins > 0) {
            std::time_t step   = static_cast<std::time_t>(block.align_to_mins) * 60;
            std::time_t prev_b = (pass.t / step) * step;
            std::time_t next_b = prev_b + step;
            bool in_early = (next_b - pass.t) <= static_cast<std::time_t>(block.early_start_secs);
            bool in_late  = (pass.t - prev_b)  <= static_cast<std::time_t>(block.late_start_mins) * 60;
            if (!in_early && !in_late) pass.t = next_b;
        }

        if (prog_limit_hit) {
            if (!block.outro_content_id.empty())
                scheduleBumperItem(ctx.channel_id, block.block_id,
                                   block.outro_content_type, block.outro_content_id,
                                   block.block_id + ":outro", ctx.result, pass.t, ctx.state);
            if (block.align_to_mins > 0) {
                auto        tm_e  = toChannelTZ(t_prog_end, ctx.tz);
                int         c     = tm_e.tm_hour * 3600 + tm_e.tm_min * 60 + tm_e.tm_sec;
                std::time_t mid   = t_prog_end - static_cast<std::time_t>(c);
                std::time_t step  = static_cast<std::time_t>(block.align_to_mins) * 60;
                pass.t = mid + ((static_cast<std::time_t>(c) + step - 1) / step) * step;
            }
            if (epgDebug())
                std::cout << "[epg] exhausted t=" << pass.t
                          << " block=" << block.block_id.substr(0,8)
                          << " prog_count=" << prog_count << '\n';
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────

void RuleEngine::projectDay(
    const ProjectContext& ctx,
    std::time_t day_start,
    std::time_t day_end,
    int day_mask_bit,
    ProjectPassState& pass,
    std::time_t t_end)
{
    std::unordered_set<std::string> exhausted;
    std::unordered_set<std::string> intro_played;
    std::unordered_set<std::string> seed_inited;

    pass.transition_counts.clear();
    pass.last_show_id.clear();
    pass.prev_block_id.clear();

    // Pre-populate exhausted from already-written scheduled_program rows.
    for (const auto& b : ctx.blocks) {
        if (b.program_count <= 0) continue;
        if (!(b.day_mask & day_mask_bit)) continue;
        SQLite::Statement q(db_.get(), R"(
            SELECT COUNT(*) FROM scheduled_program
             WHERE channel_id = ? AND block_id = ?
               AND wall_clock_start >= ? AND wall_clock_start < ?
        )");
        q.bind(1, ctx.channel_id); q.bind(2, b.block_id);
        q.bind(3, static_cast<int64_t>(day_start));
        q.bind(4, static_cast<int64_t>(day_end));
        q.executeStep();
        if (q.getColumn(0).getInt() >= b.program_count) {
            exhausted.insert(b.block_id);
            if (epgDebug())
                std::cout << "[epg] pre-pop exhausted block=" << b.block_id.substr(0,8) << '\n';
        }
    }

    // Tomorrow's day-mask bit: used when computing cross-midnight preemption windows.
    const int tom_bit = (day_mask_bit < 64) ? day_mask_bit << 1 : 1;

    // Dispatch loop. Exits when pass.t reaches day_end (dispatch boundary) or t_end.
    // scheduleBlock() may advance pass.t past day_end — that is intentional; content
    // is never hard-cut at midnight.
    while (pass.t < day_end && pass.t < t_end) {
        std::vector<Block> active;
        active.reserve(ctx.blocks.size());
        for (const auto& b : ctx.blocks)
            if (!exhausted.contains(b.block_id)) active.push_back(b);

        auto block_opt = resolveFromList(active, pass.t, ctx.tz);

        if (!block_opt) {
            // Jump toward the nearest upcoming block start to avoid 60-second crawl.
            std::time_t jump = std::min(pass.t + 1800, day_end);
            for (const auto& b : ctx.blocks) {
                if (b.day_mask & day_mask_bit) {
                    std::time_t cand = day_start
                        + static_cast<std::time_t>(parseTimeMins(b.start_time)) * 60;
                    if (cand > pass.t && cand < jump) jump = cand;
                }
            }
            // No block covers this stretch — materialize channel-level filler into the
            // gap via the same deterministic path as block-internal filler (rotation,
            // smart_pct cooldown), instead of leaving it unscheduled for the live "/now"
            // path to fill ad hoc with an ungoverned random pick.
            fillToTime(ctx, ctx.gap_block, jump, pass);
            pass.prev_block_id.clear();
            continue;
        }
        const Block& block = *block_opt;

        // ── Block-entry alignment ─────────────────────────────────────────────
        if (block.block_id != pass.prev_block_id && block.start_scope != "episode") {
            std::time_t c_sec = pass.t - day_start;
            std::time_t eff   = pass.t;
            if (block.align_to_mins > 0) {
                std::time_t step = static_cast<std::time_t>(block.align_to_mins) * 60;
                std::time_t ls   = ((c_sec + step - 1) / step) * step;
                eff = day_start + ls;
            }
            std::time_t nom = day_start
                + static_cast<std::time_t>(parseTimeMins(block.start_time)) * 60;
            std::time_t lwe = nom + static_cast<std::time_t>(block.late_start_mins) * 60;
            if (lwe > ctx.proj_start && eff > lwe) {
                std::time_t skip_to = pass.t + 1800;
                if (block.end_time.has_value()) {
                    std::time_t bend = day_start
                        + static_cast<std::time_t>(parseTimeMins(*block.end_time)) * 60;
                    if (bend > pass.t) skip_to = bend + 1;
                }
                pass.t = skip_to;
                continue;
            }
            pass.prev_block_id = block.block_id;
            pass.transition_counts[block.block_id] = 0;
            if (eff > pass.t) { pass.t = eff; continue; }
        } else {
            if (block.block_id != pass.prev_block_id) {
                pass.transition_counts[block.block_id] = 0;
                pass.last_show_id.clear();
            }
            pass.prev_block_id = block.block_id;
        }

        // ── Seed-derived starting positions (once per block per day) ──────────
        // Timeslot blocks carry per-slot cursors; no block-level seed init needed.
        if (!seed_inited.contains(block.block_id)) {
            seed_inited.insert(block.block_id);
            if (block.block_type != BlockType::Timeslot &&
                !ctx.state.hasBlockPosition(block.block_id))
            {
                if (isRerunMode(block)) {
                    bool global    = (block.cursor_scope == CursorScope::Global);
                    int  sel       = selectWeighted(block, ctx.rng);
                    const auto& sel_bc = block.content[sel];
                    ctx.state.setBlockPosition(block.block_id, sel,
                                               std::max(1, sel_bc.run_count), 0);

                    std::string sc = scopeStr(block), sc_id = scopeId(block, ctx.channel_id);
                    for (const auto& bc : block.content) {
                        if (bc.content_type != "show") continue;
                        auto played = (block.advancement == Advancement::Smart)
                            ? getPlayedEpisodesWithCooldown(bc.content_id, ctx.channel_id,
                                bc.season_filter, block.smart_pct, pass.t, global, bc.include_specials)
                            : getPlayedEpisodes(bc.content_id, ctx.channel_id, bc.season_filter,
                                pass.t, global, bc.include_specials, bc.episode_order);
                        auto all = getEpisodes(bc.content_id, bc.season_filter,
                                               bc.include_specials, bc.episode_order);

                        std::optional<SeedCursor> seed;
                        switch (block.no_history_behavior) {
                            case NoHistoryBehavior::Normal:
                                seed = getContentNormalHistory(played, all, block.snap_to_group_start, ctx.rng);
                                break;
                            case NoHistoryBehavior::FallbackAll:
                                seed = getContentFallbackHistory(played, all, block.snap_to_group_start, ctx.rng);
                                break;
                            case NoHistoryBehavior::Exclude:
                                seed = getContentExcludeHistory(played, all, block.snap_to_group_start, ctx.rng);
                                break;
                            case NoHistoryBehavior::Skip:
                                seed = getContentSkipHistory(played, all, block.snap_to_group_start, ctx.rng);
                                break;
                        }
                        if (seed)
                            ctx.state.setCursorPos(seed->content_type, bc.content_id, sc, sc_id,
                                                   seed->position, seed->episode_id);
                    }
                } else {
                    std::string sc = scopeStr(block), sc_id = scopeId(block, ctx.channel_id);
                    for (const auto& bc : block.content) {
                        if (bc.content_type != "show") continue;
                        auto eps = getEpisodes(bc.content_id, bc.season_filter,
                                               bc.include_specials, bc.episode_order);
                        if (eps.empty()) continue;
                        int pos = static_cast<int>(ctx.rng() % eps.size());
                        ctx.state.setCursorPos("show", bc.content_id, sc, sc_id,
                                               pos, eps[pos].episode_id);
                    }
                }
            }
        }

        // ── Preemption window for this block occurrence ───────────────────────
        // Bounded by the next higher-priority block's start (today or tomorrow morning).
        // Fallback is t_end, NOT day_end — items complete naturally past midnight.
        // Also track that block's late_start_mins: content may finish into that grace.
        std::time_t block_window      = t_end;
        int         block_window_late = 0;
        for (const auto& b : ctx.blocks) {
            if (b.block_id == block.block_id) break;
            auto check = [&](int bit, std::time_t base) {
                if (!(b.day_mask & bit)) return;
                std::time_t bs = base
                    + static_cast<std::time_t>(parseTimeMins(b.start_time)) * 60;
                if (bs > pass.t && bs < block_window) {
                    block_window      = bs;
                    block_window_late = b.late_start_mins;
                }
            };
            check(day_mask_bit, day_start);
            check(tom_bit,      day_end);   // day_end == tomorrow's midnight
        }

        bool first_entry = !intro_played.contains(block.block_id);
        if (first_entry) intro_played.insert(block.block_id);

        bool block_exhausted = scheduleBlock(ctx, block, block_window, block_window_late,
                                             first_entry, day_start, pass);
        if (block_exhausted) {
            exhausted.insert(block.block_id);
            intro_played.erase(block.block_id);
            pass.prev_block_id.clear();
        }
        // pass.t advanced by scheduleBlock. If past day_end the while condition exits
        // and the outer project() loop recomputes the next day's context.
    }
}

// ── Forward projection ────────────────────────────────────────────────────────

std::vector<ScheduledItem> RuleEngine::project(const std::string& channel_id,
                                                std::time_t start, int horizon_hours,
                                                CursorState& state,
                                                Xoshiro256& rng,
                                                std::map<std::time_t, std::string>* anchors_out,
                                                std::vector<PlayRecord>* play_records_out) {
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

    int rerun_min = 0;
    std::string gap_filler_selection = "round_robin";
    {
        SQLite::Statement sq(db_.get(),
            "SELECT rerun_min_time_mins, default_filler_selection FROM channel WHERE channel_id = ?");
        sq.bind(1, channel_id);
        if (sq.executeStep()) {
            rerun_min             = sq.getColumn(0).getInt();
            gap_filler_selection  = sq.getColumn(1).getString();
        }
    }

    Block gap_block;
    gap_block.filler_selection = gap_filler_selection;

    ProjectContext ctx{
        channel_id, blocks, channel_filler, between_bumpers,
        tz, start, rerun_min, result, state, rng, anchors_out, gap_block
    };

    ProjectPassState pass;
    pass.t = start;
    pass.anchor_next_monday = [&]() -> std::time_t {
        std::time_t d   = start / 86400;
        std::time_t dow = (d + 3) % 7;   // 0 = Mon
        return (d - dow + 7) * 86400;    // always the coming Monday, never start itself
    }();

    const std::time_t t_end = start + static_cast<std::time_t>(horizon_hours) * 3600;

    while (pass.t < t_end) {
        auto        tm_t      = toChannelTZ(pass.t, tz);
        int         c_sec     = tm_t.tm_hour * 3600 + tm_t.tm_min * 60 + tm_t.tm_sec;
        std::time_t day_start = pass.t - static_cast<std::time_t>(c_sec);

        // Next local midnight: 25h past day_start always crosses the boundary safely.
        std::time_t approx  = day_start + 90000;
        auto        tm_n    = toChannelTZ(approx, tz);
        int         n_s     = tm_n.tm_hour * 3600 + tm_n.tm_min * 60 + tm_n.tm_sec;
        std::time_t day_end = approx - static_cast<std::time_t>(n_s);

        int day_mask_bit = dayBit(tm_t);

        projectDay(ctx, day_start, day_end, day_mask_bit, pass, t_end);

        // Guard: if projectDay left pass.t before day_end (no active blocks, gap
        // exhausted), advance to day_end to prevent re-entering the same day.
        if (pass.t < day_end) pass.t = day_end;
    }

    if (epgDebug() || result.empty())
        std::cout << "[epg] project() channel=" << channel_id
                  << " => " << result.size() << " items"
                  << (result.empty() ? " (EMPTY — no content scheduled)" : "") << '\n';

    if (play_records_out) *play_records_out = std::move(pass.play_records);
    return result;
}

// ── Timeslot block scheduling ─────────────────────────────────────────────────

void RuleEngine::fillToTime(const ProjectContext& ctx,
                             const Block& block,
                             std::time_t target,
                             ProjectPassState& pass) {
    if (pass.t >= target) { pass.t = target; return; }
    const auto& pool = block.filler_entries.empty() ? ctx.channel_filler
                                                     : block.filler_entries;
    if (pool.empty()) { pass.t = target; return; }
    int guard = 0;
    while (pass.t < target && guard++ < 2000) {
        int64_t max_ms = (target - pass.t) * 1000;
        auto fi = pickFillerSim(ctx.channel_id, block, pool, max_ms, ctx.state, ctx.rng, pass.play_records, pass.t);
        if (!fi || fi->duration_ms <= 0 || fi->duration_ms > max_ms) break;
        fi->wall_clock_start_ms = static_cast<int64_t>(pass.t) * 1000;
        fi->wall_clock_end_ms   = fi->wall_clock_start_ms + fi->duration_ms;
        fi->cursor_json         = "{}";
        fi->channel_id          = ctx.channel_id;
        fi->block_id            = block.block_id;
        int64_t dur = fi->duration_ms;
        ctx.result.push_back(std::move(*fi));
        pass.t += dur / 1000;
    }
    if (pass.t < target) pass.t = target;
}

bool RuleEngine::scheduleTimeslotBlock(
    const ProjectContext& ctx,
    const Block& block,
    std::time_t window_end,
    int window_late_start_mins,
    std::time_t day_start,
    ProjectPassState& pass)
{
    std::time_t block_nominal_start = day_start
        + static_cast<std::time_t>(parseTimeMins(block.start_time)) * 60;

    std::time_t block_end_ts = std::numeric_limits<std::time_t>::max();
    if (block.end_time.has_value()) {
        int end_secs   = parseTimeMins(*block.end_time) * 60;
        int start_secs = parseTimeMins(block.start_time) * 60;
        std::time_t base = (end_secs < start_secs) ? day_start + 86400 : day_start;
        block_end_ts = base + static_cast<std::time_t>(end_secs);
    } else if (!block.slots.empty()) {
        // No explicit end_time: derive implicit end from the last slot's absolute end.
        // This prevents the trailing fillToTime from running to t_end after slots complete.
        const auto& last = block.slots.back();
        block_end_ts = block_nominal_start
            + static_cast<std::time_t>(last.slot_offset_mins + last.slot_duration_mins) * 60;
    }

    // loop_end: strict upper bound used for filler targets and slot start/skip checks.
    // slot_cap: softer bound that lets in-progress episodes finish into the preempting
    //           block's late-start grace window (same grace applied in scheduleBlock).
    const std::time_t window_soft = window_end
        + static_cast<std::time_t>(window_late_start_mins) * 60;
    const std::time_t loop_end = std::min(window_end,  block_end_ts);
    const std::time_t slot_cap = std::min(window_soft, block_end_ts);

    bool any_slot_ran = false;

    for (const auto& slot : block.slots) {
        std::time_t slot_start = block_nominal_start
            + static_cast<std::time_t>(slot.slot_offset_mins) * 60;
        std::time_t slot_end   = slot_start
            + static_cast<std::time_t>(slot.slot_duration_mins) * 60;
        slot_end = std::min(slot_end, slot_cap);

        if (slot_end <= pass.t) continue;    // entirely in the past
        if (slot_start >= loop_end) break;   // beyond block window

        // This slot is in the current window; mark the block as active for this pass.
        any_slot_ran = true;

        // Fill gap before this slot with bumpers/filler.
        if (pass.t < slot_start) fillToTime(ctx, block, slot_start, pass);

        // Late-start check: skip slot if we've blown past its entry window.
        std::time_t lwe = slot_start
            + static_cast<std::time_t>(slot.late_start_mins) * 60;
        if (pass.t > lwe) { pass.t = slot_end; continue; }

        // Slot entry alignment.
        if (slot.start_scope != "episode" && slot.align_to_mins > 0) {
            std::time_t step  = static_cast<std::time_t>(slot.align_to_mins) * 60;
            std::time_t c_sec = pass.t - day_start;
            std::time_t ls    = ((c_sec + step - 1) / step) * step;
            std::time_t eff   = day_start + ls;
            if (eff > pass.t && eff < slot_end) pass.t = eff;
        }

        // Schedule content.
        auto sc = ctx.state.getSlotCursor(slot.slot_id);
        int sel_ts = pickNextContent(ctx.channel_id, block, pass.t, ctx.state, pass.play_records, ctx.rng);
        auto item_opt = (sel_ts >= 0)
            ? advanceAndGet(ctx.channel_id, block, sel_ts, pass.t, ctx.state, pass.play_records, ctx.rng)
            : std::nullopt;
        if (item_opt) {
            int64_t dur_ms = item_opt->duration_ms;
            if (slot.overflow == SlotOverflow::Cutoff) {
                int64_t rem = (slot_end - pass.t) * 1000;
                if (rem > 0) dur_ms = std::min(dur_ms, rem);
            }
            item_opt->wall_clock_start_ms = static_cast<int64_t>(pass.t) * 1000;
            item_opt->wall_clock_end_ms   = item_opt->wall_clock_start_ms + dur_ms;
            item_opt->cursor_json         = "{}";
            item_opt->channel_id          = ctx.channel_id;
            item_opt->block_id            = block.block_id;
            pass.play_records.push_back({ctx.channel_id, item_opt->item_type, item_opt->item_id, item_opt->show_id, block.block_id, pass.t});
            ctx.state.addPlayRecord(ctx.channel_id, item_opt->item_type, item_opt->item_id, item_opt->show_id, block.block_id, pass.t);

            // Cull pass.play_records based on rerun_min_time_mins.
            int effective_min = block.rerun_min_time_mins > 0 ? block.rerun_min_time_mins : ctx.rerun_min_time_mins;
            if (effective_min > 0) {
                std::time_t threshold = pass.t - static_cast<std::time_t>(effective_min) * 60;
                pass.play_records.erase(
                    std::remove_if(pass.play_records.begin(), pass.play_records.end(),
                                   [&](const PlayRecord& pr) { return pr.aired_at < threshold; }),
                    pass.play_records.end());
            }
            ctx.result.push_back(std::move(*item_opt));
            pass.t += dur_ms / 1000;

            sc.episode_pos++;
            ctx.state.setSlotCursor(slot.slot_id, sc.queue_pos, sc.episode_pos);
        } else {
            if (slot.overflow == SlotOverflow::Cutoff) pass.t = slot_end;
        }

        // After content: fill remainder of slot with filler (cutoff mode),
        // or leave pass.t where it is (finish mode — episode ran over).
        if (slot.overflow == SlotOverflow::Cutoff && pass.t < slot_end)
            fillToTime(ctx, block, slot_end, pass);
    }

    // Fill time between the last slot and block/window end — only when the block
    // actually ran content this pass. If all slots were already in the past we signal
    // exhaustion instead; the outer projectDay loop won't re-enter the block today.
    if (any_slot_ran) {
        if (pass.t < loop_end) fillToTime(ctx, block, loop_end, pass);
        // Guard: if the filler pool was empty and couldn't advance pass.t, force it to
        // loop_end so projectDay moves on rather than spinning on this block.
        if (pass.t < loop_end) pass.t = loop_end;
    }

    // Return true (exhausted) when no slots were active this pass so projectDay
    // removes the block from the active set for the rest of this calendar day.
    return !any_slot_ran;

    return false; // timeslot blocks don't exhaust via program_count
}

// ── Playback completion ───────────────────────────────────────────────────────

void RuleEngine::markPlayed(const std::string& channel_id, const std::string& block_id,
                              const std::string& item_type, const std::string& item_id,
                              int64_t /*duration_ms*/) {
    SQLite::Transaction txn(db_.get());

    ScheduleRepository(db_).recordPlayHistory(item_type, item_id, channel_id, block_id);

    // In 'scheduled' mode project() already advanced cursors during EPG generation,
    // so advancing again here would double-advance and skip episodes on the next
    // ensureScheduled() pass. Only advance on confirmed play for 'on_play' channels.
    if (!block_id.empty() && channelAdvanceMode(channel_id) == "on_play") {
        auto b = blocks_.loadBlock(block_id);
        if (b) {
            CursorState cs = CursorRepository(db_).load(channel_id);
            Xoshiro256 rng(std::hash<std::string>{}(channel_id + block_id)
                           ^ static_cast<uint64_t>(std::time(nullptr)));
            int sel = pickNextContent(channel_id, *b, std::time(nullptr), cs, {}, rng);
            if (sel >= 0) advanceAndGet(channel_id, *b, sel, std::time(nullptr), cs, {}, rng);
            CursorRepository(db_).apply(channel_id, cs);
        }
    }

    txn.commit();
}
