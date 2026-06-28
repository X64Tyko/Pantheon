#include "RuleEngine.h"
#include "../db/BlockRepository.h"
#include "../db/ContentRepository.h"
#include "../db/CursorRepository.h"
#include "../db/Database.h"
#include "../db/ScheduleRepository.h"
#include "../db/TimeslotRepository.h"
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

    auto hot_ids = content_.getHotMovieIds(channel_id, before_time, hot_count);
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
    std::time_t before_time)
{
    int n = static_cast<int>(all.size());
    int hot_count = std::max(0, n * smart_pct / 100);
    if (hot_count == 0 || all.empty()) return all;

    auto hot_ids = content_.getHotEpisodeIds(channel_id, before_time, show_id, hot_count);
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
    return advanceAndGet(channel_id, block, before_time, state, dummy_rng);
}

// ── advanceAndGet: select + advance in one pass ───────────────────────────────
// Queries the episode pool once and both selects the current item and advances
// all cursor state in CursorState. Returns nullopt (without advancing) when no
// item is available. Replaces the old nextItem(private)+advanceCursors pair.

std::optional<ScheduledItem> RuleEngine::advanceAndGet(
    const std::string& channel_id, const Block& block,
    std::time_t before_time, CursorState& state, Xoshiro256& rng)
{
    if (block.content.empty()) return std::nullopt;

    const int   n   = static_cast<int>(block.content.size());
    const int   content_pos  = state.getContentPosition(block.block_id) % n;
    const auto& entry  = block.content[content_pos];

    std::optional<ScheduledItem> item;

    // ── Show ─────────────────────────────────────────────────────────────────────
    if (entry.content_type == "show") {
        const std::string scope    = scopeStr(block);
        const std::string scope_id = scopeId(block, channel_id);

        if (isRerunMode(block)) {
            bool global = (block.cursor_scope == CursorScope::Global);
            auto eps = (block.advancement == Advancement::Smart)
                ? getPlayedEpisodesWithCooldown(entry.content_id, channel_id, entry.season_filter,
                                                block.smart_pct, before_time, global, entry.include_specials)
                : getPlayedEpisodes(entry.content_id, channel_id, entry.season_filter,
                                    before_time, global, entry.include_specials, entry.episode_order);

            if (block.no_history_behavior == NoHistoryBehavior::Normal) {
                auto all = getEpisodes(entry.content_id, entry.season_filter,
                                       entry.include_specials, entry.episode_order);
                if (all.empty()) return std::nullopt;
                int seq_pos = state.getCursorPos("show", entry.content_id, scope, scope_id);

                if (block.advancement == Advancement::Smart) {
                    item = itemFromShow(channel_id, block.block_id, all,
                                        seq_pos, showTitle(entry.content_id));
                    int next_pos = (seq_pos + 1) % static_cast<int>(all.size());
                    if (!eps.empty()) {
                        std::unordered_set<std::string> aired;
                        aired.reserve(eps.size());
                        for (const auto& e : eps) aired.insert(e.episode_id);
                        if (!aired.contains(all[next_pos].episode_id)) next_pos = 0;
                    }
                    state.setCursorPos("show", entry.content_id, scope, scope_id,
                                       next_pos, all[next_pos].episode_id);
                } else if (eps.empty() || seq_pos >= static_cast<int>(eps.size())) {
                    // RerunShuffle first-run
                    item = itemFromShow(channel_id, block.block_id, all,
                                        seq_pos, showTitle(entry.content_id));
                    int next_pos = (seq_pos + 1) % static_cast<int>(all.size());
                    state.setCursorPos("show", entry.content_id, scope, scope_id,
                                       next_pos, all[next_pos].episode_id);
                } else {
                    // RerunShuffle first-run done: enter rerun shuffle pool
                    int pos   = state.getCursorPos("show_rerun", entry.content_id, scope, scope_id);
                    auto perm = groupedShufflePermutation(entry.content_id + block.block_id, eps);
                    item = itemFromShow(channel_id, block.block_id, eps,
                                        perm[pos % static_cast<int>(perm.size())],
                                        showTitle(entry.content_id));
                    int next_pos = (pos + 1) % static_cast<int>(eps.size());
                    state.setCursorPos("show_rerun", entry.content_id, scope, scope_id,
                                       next_pos, eps[next_pos].episode_id);
                }
            } else if (!eps.empty()) {
                int pos   = state.getCursorPos("show_rerun", entry.content_id, scope, scope_id);
                auto perm = groupedShufflePermutation(entry.content_id + block.block_id, eps);
                item = itemFromShow(channel_id, block.block_id, eps,
                                    perm[pos % static_cast<int>(perm.size())],
                                    showTitle(entry.content_id));
                int next_pos = (pos + 1) % static_cast<int>(eps.size());
                state.setCursorPos("show_rerun", entry.content_id, scope, scope_id,
                                   next_pos, eps[next_pos].episode_id);
            } else if (block.no_history_behavior == NoHistoryBehavior::FallbackAll) {
                auto all = getEpisodes(entry.content_id, entry.season_filter,
                                       entry.include_specials, entry.episode_order);
                if (!all.empty()) {
                    int pos   = state.getCursorPos("show_rerun", entry.content_id, scope, scope_id);
                    auto perm = groupedShufflePermutation(entry.content_id + block.block_id, all);
                    item = itemFromShow(channel_id, block.block_id, all,
                                        perm[pos % static_cast<int>(perm.size())],
                                        showTitle(entry.content_id));
                    int next_pos = (pos + 1) % static_cast<int>(all.size());
                    state.setCursorPos("show_rerun", entry.content_id, scope, scope_id,
                                       next_pos, all[next_pos].episode_id);
                }
            }
            // else Skip: item stays nullopt, no cursor advance

            // ── Block-level show selection ────────────────────────────────────────
            int runs_remaining = state.getRunsRemaining(block.block_id);
            int consecutive   = state.getConsecutiveCount(block.block_id) + 1;

            if (runs_remaining <= 1) {
                int next_sel = -1; // -1: Exclude path cleared eligible list, skip selection
                if (block.no_history_behavior == NoHistoryBehavior::Exclude) {
                    std::vector<int> eligible;
                    int total_w = 0;
                    for (int i = 0; i < n; i++) {
                        const auto& c_entry = block.content[i];
                        if (c_entry.content_type == "show") {
                            auto ceps = (block.advancement == Advancement::Smart)
                                ? getPlayedEpisodesWithCooldown(c_entry.content_id, channel_id,
                                    c_entry.season_filter, block.smart_pct, before_time,
                                    global, c_entry.include_specials)
                                : getPlayedEpisodes(c_entry.content_id, channel_id, c_entry.season_filter,
                                    before_time, global, c_entry.include_specials, c_entry.episode_order);
                            if (ceps.empty()) continue;
                        }
                        eligible.push_back(i);
                        total_w += std::max(1, c_entry.weight);
                    }
                    if (eligible.empty()) {
                        state.setBlockPosition(block.block_id, 0, 0, 0);
                    } else {
                        std::uniform_int_distribution<int> dist(0, total_w - 1);
                        int r = dist(rng);
                        next_sel = eligible.back();
                        for (int idx : eligible) {
                            r -= std::max(1, block.content[idx].weight);
                            if (r < 0) { next_sel = idx; break; }
                        }
                    }
                } else {
                    next_sel = selectWeighted(block, rng);
                }

                if (next_sel >= 0) {
                    bool same_show = (next_sel == content_pos);
                    bool limit_hit = (block.max_consecutive_episodes > 0 &&
                                      consecutive >= block.max_consecutive_episodes);
                    if (same_show && limit_hit && n > 1) {
                        int total_w = 0;
                        for (int i = 0; i < n; ++i)
                            if (i != content_pos) total_w += std::max(1, block.content[i].weight);
                        if (total_w > 0) {
                            std::uniform_int_distribution<int> dist(0, total_w - 1);
                            int r = dist(rng);
                            for (int i = 0; i < n; ++i) {
                                if (i == content_pos) continue;
                                r -= std::max(1, block.content[i].weight);
                                if (r < 0) { next_sel = i; break; }
                            }
                            same_show = false;
                        }
                    }
                    const auto& next_entry  = block.content[next_sel];
                    int         next_run = std::max(1, next_entry.run_count);
                    if (!same_show || limit_hit) {
                        if (block.advancement == Advancement::Smart &&
                            block.no_history_behavior == NoHistoryBehavior::Normal) {
                            auto all_next = getEpisodes(next_entry.content_id, next_entry.season_filter,
                                                        next_entry.include_specials, next_entry.episode_order);
                            if (!all_next.empty()) {
                                auto next_eps = getPlayedEpisodesWithCooldown(
                                    next_entry.content_id, channel_id, next_entry.season_filter,
                                    block.smart_pct, before_time, global, next_entry.include_specials);
                                if (next_eps.empty()) {
                                    // No premiers: random entry into full catalog + snap (hook behavior).
                                    // Part 1 plays before Part 2 even when Part 2 is the random pick.
                                    std::uniform_int_distribution<int> dist(0, static_cast<int>(all_next.size()) - 1);
                                    int start = dist(rng);
                                    int snap  = block.snap_to_group_start
                                        ? snapToGroupStart(all_next[start].episode_id, all_next) : -1;
                                    int final_pos = (snap >= 0) ? snap : start;
                                    state.setCursorPos("show", next_entry.content_id, scope, scope_id,
                                                       final_pos, all_next[final_pos].episode_id);
                                } else {
                                    // Has premiers: pick from played pool. Snap to Part 1 only if
                                    // Part 1 is also in the pool (snapToGroupStart returns -1 otherwise).
                                    std::uniform_int_distribution<int> dist(0, static_cast<int>(next_eps.size()) - 1);
                                    int start = dist(rng);
                                    int snap_in_pool = block.snap_to_group_start
                                        ? snapToGroupStart(next_eps[start].episode_id, next_eps) : -1;
                                    const std::string& target_id = (snap_in_pool >= 0)
                                        ? next_eps[snap_in_pool].episode_id : next_eps[start].episode_id;
                                    int pos_in_all = 0;
                                    for (int i = 0; i < (int)all_next.size(); ++i)
                                        if (all_next[i].episode_id == target_id) { pos_in_all = i; break; }
                                    state.setCursorPos("show", next_entry.content_id, scope, scope_id,
                                                       pos_in_all, all_next[pos_in_all].episode_id);
                                }
                            }
                        } else {
                            auto next_eps = (block.advancement == Advancement::Smart)
                                ? getPlayedEpisodesWithCooldown(next_entry.content_id, channel_id,
                                    next_entry.season_filter, block.smart_pct, before_time,
                                    global, next_entry.include_specials)
                                : getPlayedEpisodes(next_entry.content_id, channel_id, next_entry.season_filter,
                                    before_time, global, next_entry.include_specials, next_entry.episode_order);
                            if (next_eps.empty() && block.no_history_behavior == NoHistoryBehavior::FallbackAll)
                                next_eps = getEpisodes(next_entry.content_id, next_entry.season_filter,
                                                       next_entry.include_specials, next_entry.episode_order);
                            if (!next_eps.empty()) {
                                std::uniform_int_distribution<int> dist(0, static_cast<int>(next_eps.size()) - 1);
                                int start = dist(rng);
                                int snap = block.snap_to_group_start
                                    ? snapToGroupStart(next_eps[start].episode_id, next_eps) : -1;
                                int final_start = (snap >= 0) ? snap : start;
                                state.setCursorPos("show_rerun", next_entry.content_id, scope, scope_id,
                                                   final_start, next_eps[final_start].episode_id);
                            }
                        }
                        state.setBlockPosition(block.block_id, next_sel, next_run, 0);
                    } else {
                        state.setBlockPosition(block.block_id, next_sel, next_run, consecutive);
                    }
                }
            } else {
                state.setBlockPosition(block.block_id,
                                       state.getContentPosition(block.block_id),
                                       runs_remaining - 1, consecutive);
            }
            // Rerun show falls through to global content-pos advance below.

        } else {
            // ── Non-rerun show (Sequential / Shuffle / SmartShuffle) ───────────────
            auto all_eps = getEpisodes(entry.content_id, entry.season_filter,
                                       entry.include_specials, entry.episode_order);
            if (all_eps.empty()) return std::nullopt;

            int pos = state.getCursorPos("show", entry.content_id, scope, scope_id);
            int next_pos;

            if (block.advancement == Advancement::Smart && block.smart_pct > 0) {
                auto eps  = smartShufflePool(all_eps, entry.content_id, channel_id,
                                             block.smart_pct, before_time);
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

            if (block.advancement == Advancement::Shuffle ||
                block.advancement == Advancement::Smart) {
                state.setContentPosition(block.block_id, selectWeighted(block, rng));
            } else {
                int runs_remaining = state.getRunsRemaining(block.block_id);
                if (runs_remaining == 0) runs_remaining = std::max(1, entry.weight);
                if (runs_remaining <= 1) {
                    int next_content_pos   = (content_pos + 1) % n;
                    int next_runs = std::max(1, block.content[next_content_pos].weight);
                    state.setBlockPosition(block.block_id, next_content_pos, next_runs, 0);
                } else {
                    state.setBlockPosition(block.block_id, content_pos, runs_remaining - 1, 0);
                }
            }
            return item; // Non-rerun show: skip global content-pos advance

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
                bool global = (block.cursor_scope == CursorScope::Global);
                auto eps = (block.advancement == Advancement::Smart)
                    ? getPlayedEpisodesWithCooldown(show_id, channel_id, std::nullopt,
                                                    block.smart_pct, before_time, global)
                    : getPlayedEpisodes(show_id, channel_id, std::nullopt, before_time, global);
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

                int runs_remaining = state.getRunsRemaining(block.block_id);
                int consecutive   = state.getConsecutiveCount(block.block_id) + 1;

                if (runs_remaining <= 1) {
                    int next_idx;
                    if (n_shows == 1) {
                        next_idx = 0;
                    } else {
                        std::uniform_int_distribution<int> dist(0, n_shows - 2);
                        int r = dist(rng);
                        next_idx = (r >= show_idx) ? r + 1 : r;
                    }
                    bool same_show = (next_idx == show_idx);
                    bool limit_hit = (block.max_consecutive_episodes > 0 &&
                                      consecutive >= block.max_consecutive_episodes);
                    int next_run = std::max(1, entry.run_count);
                    if (!same_show || limit_hit) {
                        const std::string& next_show = shows[next_idx];
                        auto next_eps = (block.advancement == Advancement::Smart)
                            ? getPlayedEpisodesWithCooldown(next_show, channel_id, std::nullopt,
                                                            block.smart_pct, before_time, global)
                            : getPlayedEpisodes(next_show, channel_id, std::nullopt,
                                                before_time, global);
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
                        state.setCursorPos("playlist", entry.content_id, scope, scope_id, next_idx);
                        state.setBlockPosition(block.block_id, content_pos, next_run, 0);
                    } else {
                        state.setCursorPos("playlist", entry.content_id, scope, scope_id, next_idx);
                        state.setBlockPosition(block.block_id, content_pos, next_run, consecutive);
                    }
                } else {
                    state.setBlockPosition(block.block_id, content_pos, runs_remaining - 1, consecutive);
                }
            } else {
                // Non-rerun show_collection
                auto pl_eps = getPlaylistShowEpisodes(entry.content_id, show_id);
                if (!pl_eps.empty()) {
                    int pos = state.getCursorPos("show", show_id, scope, scope_id);
                    int next_pos;
                    if (block.advancement == Advancement::Smart && block.smart_pct > 0) {
                        auto filt = smartShufflePool(pl_eps, show_id, channel_id,
                                                      block.smart_pct, before_time);
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
                if (block.advancement == Advancement::Shuffle ||
                    block.advancement == Advancement::Smart) {
                    std::uniform_int_distribution<int> dist(0, n_shows - 1);
                    state.setCursorPos("playlist", entry.content_id, scope, scope_id, dist(rng));
                } else {
                    int runs_remaining = state.getRunsRemaining(block.block_id);
                    if (runs_remaining == 0) runs_remaining = std::max(1, entry.weight);
                    if (runs_remaining <= 1) {
                        int next_idx  = (show_idx + 1) % n_shows;
                        int next_runs = std::max(1, entry.weight);
                        state.setCursorPos("playlist", entry.content_id, scope, scope_id, next_idx);
                        state.setBlockPosition(block.block_id, content_pos, next_runs, 0);
                    } else {
                        state.setBlockPosition(block.block_id, content_pos, runs_remaining - 1, 0);
                    }
                }
            }
            // show_collection: fall through to global content-pos advance

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
            bool is_fl = (entry.content_type == "filler_list");
            const char* cnt_sql = is_fl
                ? "SELECT COUNT(*) FROM filler_list_item WHERE filler_list_id=?"
                : "SELECT COUNT(*) FROM playlist_item     WHERE playlist_id=?";
            SQLite::Statement q_cnt(db_.get(), cnt_sql);
            q_cnt.bind(1, entry.content_id);
            if (q_cnt.executeStep()) {
                int list_size = q_cnt.getColumn(0).getInt();
                if (list_size > 0)
                    state.setCursorPos(entry.content_type, entry.content_id, scope, scope_id,
                                       (pos + 1) % list_size);
            }
        }
    }

    // ── Global content-position advance ──────────────────────────────────────────
    // Runs for: movie, episode, flat playlist/filler, show_collection, and rerun show.
    // Non-rerun show returns early above to manage its own content-pos advance.
    if (block.advancement == Advancement::Smart && block.smart_pct > 0) {
        state.setContentPosition(block.block_id,
            selectWeightedSmartCooldown(block, channel_id, block.smart_pct, before_time, rng));
    } else if (block.advancement == Advancement::Shuffle ||
               block.advancement == Advancement::Smart) {
        state.setContentPosition(block.block_id, selectWeighted(block, rng));
    } else {
        state.setContentPosition(block.block_id, (content_pos + 1) % n);
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

        CursorState snap = ctx.state;
        auto item_opt = advanceAndGet(ctx.channel_id, block, pass.t, ctx.state, ctx.rng);

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
                if (auto fi = pickFillerSim(ctx.channel_id, block, pool, 0, ctx.state, ctx.rng, pass.t)) {
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

        if (!is_fallback_filler) pass.last_show_id = item.show_id;
        ctx.result.push_back(std::move(item));
        pass.t += dur_ms / 1000;
        const std::time_t t_prog_end = pass.t;

        if (!is_fallback_filler)
            pass.play_records.push_back({ctx.channel_id, ph_type, ph_show, ph_id, ph_at});

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
                        auto fi = pickFillerSim(ctx.channel_id, block, pool, rem_ms, ctx.state, ctx.rng, pass.t);
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
            pass.t = jump;
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
                    auto eps = (block.advancement == Advancement::Smart)
                        ? getPlayedEpisodesWithCooldown(sel_bc.content_id, ctx.channel_id,
                            sel_bc.season_filter, block.smart_pct, pass.t, global, sel_bc.include_specials)
                        : getPlayedEpisodes(sel_bc.content_id, ctx.channel_id, sel_bc.season_filter,
                            pass.t, global, sel_bc.include_specials, sel_bc.episode_order);
                    if (eps.empty() && block.no_history_behavior == NoHistoryBehavior::FallbackAll)
                        eps = getEpisodes(sel_bc.content_id, sel_bc.season_filter,
                                          sel_bc.include_specials, sel_bc.episode_order);
                    if (!eps.empty()) {
                        std::uniform_int_distribution<int> dist(0, (int)eps.size() - 1);
                        int sp = dist(ctx.rng);
                        int sn = block.snap_to_group_start
                            ? snapToGroupStart(eps[sp].episode_id, eps) : -1;
                        int fp = (sn >= 0) ? sn : sp;
                        ctx.state.setCursorPos("show_rerun", sel_bc.content_id,
                                               scopeStr(block), scopeId(block, ctx.channel_id),
                                               fp, eps[fp].episode_id);
                    }
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
                        if (!played.empty()) continue;
                        auto all = getEpisodes(bc.content_id, bc.season_filter,
                                               bc.include_specials, bc.episode_order);
                        if (all.empty()) continue;
                        int pos = static_cast<int>(ctx.rng() % all.size());
                        int sn2 = block.snap_to_group_start
                            ? snapToGroupStart(all[pos].episode_id, all) : -1;
                        int fp2 = (sn2 >= 0) ? sn2 : pos;
                        ctx.state.setCursorPos("show", bc.content_id, sc, sc_id,
                                               fp2, all[fp2].episode_id);
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

    ProjectContext ctx{
        channel_id, blocks, channel_filler, between_bumpers,
        tz, start, result, state, rng, anchors_out
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
        auto fi = pickFillerSim(ctx.channel_id, block, pool, max_ms, ctx.state, ctx.rng, pass.t);
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

std::optional<ScheduledItem> RuleEngine::pickSlotEpisode(
    const std::string& channel_id,
    const std::string& block_id,
    const TimeslotQueueEntry& entry,
    int episode_pos)
{
    if (entry.content_type == "show") {
        auto eps = getEpisodes(entry.content_id, std::nullopt, false, "season");
        if (eps.empty() || episode_pos >= static_cast<int>(eps.size()))
            return std::nullopt;
        return itemFromShow(channel_id, block_id, eps, episode_pos, showTitle(entry.content_id));
    }
    if (entry.content_type == "movie") {
        if (episode_pos > 0) return std::nullopt; // one-shot
        auto m = getMovie(entry.content_id);
        if (!m) return std::nullopt;
        auto mi = movieItem(*m);
        mi.channel_id = channel_id;
        mi.block_id   = block_id;
        return mi;
    }
    return std::nullopt;
}

void RuleEngine::scheduleTimeslotSlot(
    const ProjectContext& ctx,
    const Block& block,
    const TimeslotSlot& slot,
    std::time_t slot_end,
    ProjectPassState& pass)
{
    if (slot.queue.empty()) return;

    auto sc = ctx.state.getSlotCursor(slot.slot_id);

    // Current date as "YYYY-MM-DD" string for premiere_date comparisons.
    char date_buf[11];
    {
        auto tm_now = toChannelTZ(pass.t, ctx.tz);
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
    }
    const std::string today_str = date_buf;

    // Each slot plays exactly one program; filler/bumpers cover the remaining slot time.
    // The guard loop exists only to skip exhausted queue entries, not to pack episodes.
    int guard = 0;
    while (guard++ < 5000) {
        if (sc.queue_pos >= static_cast<int>(slot.queue.size())) break;
        const auto& entry = slot.queue[sc.queue_pos];

        // Premiere guard: if this entry hasn't arrived yet, apply pre_premiere_behavior.
        if (!entry.premiere_date.empty() && entry.premiere_date > today_str) {
            if (entry.pre_premiere_behavior == "skip") {
                pass.t = slot_end;
                break;
            }
            if (entry.pre_premiere_behavior == "filler") break; // fall through to filler
            // "replay_previous": wrap episode_pos within the PREVIOUS queue item.
            if (sc.queue_pos > 0) {
                const auto& prev = slot.queue[sc.queue_pos - 1];
                int prev_count = 0;
                if (prev.content_type == "show") {
                    auto eps = getEpisodes(prev.content_id, std::nullopt, false, "season");
                    prev_count = static_cast<int>(eps.size());
                } else if (prev.content_type == "movie") {
                    prev_count = 1;
                }
                if (prev_count > 0) sc.episode_pos = sc.episode_pos % prev_count;
                const auto& eff_entry = prev;
                auto item = pickSlotEpisode(ctx.channel_id, block.block_id, eff_entry, sc.episode_pos);
                if (!item) break;
                int64_t dur_ms = item->duration_ms;
                if (slot.overflow == SlotOverflow::Cutoff) {
                    int64_t rem = (slot_end - pass.t) * 1000;
                    if (rem <= 0) break;
                    dur_ms = std::min(dur_ms, rem);
                }
                item->wall_clock_start_ms = static_cast<int64_t>(pass.t) * 1000;
                item->wall_clock_end_ms   = item->wall_clock_start_ms + dur_ms;
                item->cursor_json         = "{}";
                item->channel_id          = ctx.channel_id;
                item->block_id            = block.block_id;
                pass.play_records.push_back({ctx.channel_id, item->item_type, item->show_id, item->item_id, pass.t});
                ctx.result.push_back(std::move(*item));
                pass.t += dur_ms / 1000;
                sc.episode_pos++;
                ctx.state.setSlotCursor(slot.slot_id, sc.queue_pos, sc.episode_pos);
            }
            break; // stop content; filler fills the rest of the slot
        }

        auto item = pickSlotEpisode(ctx.channel_id, block.block_id, entry, sc.episode_pos);
        if (!item) {
            // Queue item exhausted — only remove from DB if the show has finished airing
            // (last episode aired_at is in the past). Ongoing shows hold and fill with filler.
            bool is_complete = false;
            if (entry.content_type == "show") {
                SQLite::Statement aq(db_.get(),
                    "SELECT COALESCE(MAX(aired_at),0) FROM episode WHERE show_id=?");
                aq.bind(1, entry.content_id);
                if (aq.executeStep()) {
                    int64_t last_aired = aq.getColumn(0).getInt64();
                    is_complete = last_aired > 0 && last_aired < static_cast<int64_t>(pass.t);
                }
            } else {
                is_complete = true; // movies are always complete
            }

            if (is_complete) {
                TimeslotRepository(db_).removeExhaustedQueueEntry(entry.entry_id, slot.slot_id);
                // sc.queue_pos unchanged: next entry has shifted down to this index.
                sc.episode_pos = 0;
                ctx.state.setSlotCursor(slot.slot_id, sc.queue_pos, sc.episode_pos);
            }
            // Whether removed or holding, stop content for this slot occurrence.
            break;
        }

        int64_t dur_ms = item->duration_ms;
        if (slot.overflow == SlotOverflow::Cutoff) {
            int64_t rem = (slot_end - pass.t) * 1000;
            if (rem <= 0) break;
            dur_ms = std::min(dur_ms, rem);
        }

        item->wall_clock_start_ms = static_cast<int64_t>(pass.t) * 1000;
        item->wall_clock_end_ms   = item->wall_clock_start_ms + dur_ms;
        item->cursor_json         = "{}";
        item->channel_id          = ctx.channel_id;
        item->block_id            = block.block_id;
        pass.play_records.push_back({ctx.channel_id, item->item_type, item->show_id, item->item_id, pass.t});
        ctx.result.push_back(std::move(*item));
        pass.t += dur_ms / 1000;

        sc.episode_pos++;
        ctx.state.setSlotCursor(slot.slot_id, sc.queue_pos, sc.episode_pos);
        break; // one program per slot; scheduleTimeslotBlock fills the remainder
    }
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
        scheduleTimeslotSlot(ctx, block, slot, slot_end, pass);

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
            advanceAndGet(channel_id, *b, std::time(nullptr), cs, rng);
            CursorRepository(db_).apply(channel_id, cs);
        }
    }

    txn.commit();
}
