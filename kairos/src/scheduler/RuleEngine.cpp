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
#include <openssl/cryptoerr_legacy.h>

#include "RuntimeFlags.h"
#include "log/DebugLog.h"

using json = nlohmann::json;

// ── Portability helpers ───────────────────────────────────────────────────────

static std::tm toUTC(std::time_t t)
{
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
static std::tm toChannelTZ(std::time_t t, const std::string& tz_name)
{
	try
	{
		using namespace std::chrono;
		auto tp               = system_clock::from_time_t(t);
		const time_zone* zone = locate_zone(tz_name.empty() ? "UTC" : tz_name);
		zoned_time<system_clock::duration> zt{zone, tp};
		auto lt = zt.get_local_time();
		auto dp = floor<days>(lt);
		// Convert local_days → sys_days for year_month_day (same epoch, MSVC-safe).
		year_month_day ymd{sys_days{dp.time_since_epoch()}};

		// Time-of-day: avoid hms<> — MSVC mis-parses it as a function template.
		int total_sec = static_cast<int>(duration_cast<seconds>(lt - dp).count());
		int hour      = total_sec / 3600;
		int min       = (total_sec % 3600) / 60;
		int sec       = total_sec % 60;

		// Day of week: 1970-01-01 was a Thursday (0=Sun, so Thursday=4).
		// Shift epoch days by +4 and wrap to [0,6].
		int epoch_day = static_cast<int>(dp.time_since_epoch().count());
		int wday      = ((epoch_day % 7) + 4 + 7) % 7;

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
	}
	catch (...)
	{
		return toUTC(t);
	}
}

// ── Parsing helpers ───────────────────────────────────────────────────────────

static int parseTimeMins(const std::string& s)
{
	// "HH:MM" → minutes from midnight
	return std::stoi(s.substr(0, 2)) * 60 + std::stoi(s.substr(3, 2));
}

static int dayBit(const std::tm& tm)
{
	// Sun=1 Mon=2 Tue=4 … Sat=64 (matches day_mask convention)
	return 1 << tm.tm_wday;
}

static bool isRerunMode(const Block& block)
{
	return block.play_style == PlayStyle::Rerun;
}

// ─────────────────────────────────────────────────────────────────────────────

RuleEngine::RuleEngine(Database& db)
	: db_(db)
	, blocks_(db)
	, content_(db)
{
}

// ── Static helpers ────────────────────────────────────────────────────────────

std::string RuleEngine::scopeStr(const Block& b)
{
	if (isRerunMode(b) && b.cursor_scope == CursorScope::Block)
	{
		return "channel";
	}
	switch (b.cursor_scope)
	{
		case CursorScope::Global: return "global";
		case CursorScope::Channel: return "channel";
		case CursorScope::Block: return "block";
	}
	return "block";
}

std::string RuleEngine::scopeId(const Block& b, const std::string& channel_id)
{
	if (isRerunMode(b) && b.cursor_scope == CursorScope::Block)
	{
		return channel_id;
	}
	switch (b.cursor_scope)
	{
		case CursorScope::Global: return "";
		case CursorScope::Channel: return channel_id;
		case CursorScope::Block: return b.block_id;
	}
	return b.block_id;
}

// ── DB loading (delegates to BlockRepository / ContentRepository) ─────────────

std::vector<Block> RuleEngine::loadBlocks(const std::string& channel_id)
{
	return blocks_.loadBlocks(channel_id);
}

std::vector<Episode> RuleEngine::getEpisodes(const std::string& show_id,
											 std::optional<int> season,
											 bool include_specials,
											 const std::string& episode_order)
{
	return content_.getEpisodes(show_id, season, include_specials, episode_order);
}

std::vector<Episode> RuleEngine::getPlayedEpisodes(const std::string& show_id,
												   const std::string& channel_id,
												   std::optional<int> season,
												   std::time_t before_time,
												   bool global_scope,
												   bool include_specials,
												   const std::string& episode_order)
{
	return content_.getPlayedEpisodes(show_id, channel_id, season, before_time,
									  global_scope, include_specials, episode_order);
}

// ── Shuffle helpers ───────────────────────────────────────────────────────────

std::vector<int> RuleEngine::shufflePermutation(const std::string& seed_str, int n)
{
	std::vector<int> order(static_cast<size_t>(n));
	std::iota(order.begin(), order.end(), 0);
	Xoshiro256 rng(std::hash<std::string>{}(seed_str));
	std::shuffle(order.begin(), order.end(), rng);
	return order;
}

int RuleEngine::selectWeighted(const Block& block, Xoshiro256& rng)
{
	if (block.content.empty()) return 0;
	int total = 0;
	for (const auto& bc : block.content) total += std::max(1, bc.weight);
	std::uniform_int_distribution<int> dist(0, total - 1);
	int r = dist(rng);
	for (int i = 0; i < static_cast<int>(block.content.size()); ++i)
	{
		r -= std::max(1, block.content[i].weight);
		if (r < 0) return i;
	}
	return static_cast<int>(block.content.size()) - 1;
}

int RuleEngine::selectWeightedExcluding(const Block& block, int exclude_idx, Xoshiro256& rng)
{
	int total = 0;
	for (int i = 0; i < (int)block.content.size(); ++i) if (i != exclude_idx) total += std::max(1, block.content[i].weight);
	if (total <= 0) return selectWeighted(block, rng);
	std::uniform_int_distribution<int> dist(0, total - 1);
	int r = dist(rng);
	for (int i = 0; i < (int)block.content.size(); ++i)
	{
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
	for (const auto& bc : block.content) if (bc.content_type != "movie") return selectWeighted(block, rng);

	int hot_count = std::max(0, n * smart_pct / 100);
	if (hot_count == 0) return selectWeighted(block, rng);

	auto hot_ids = content_.getHotMovieIds(channel_id, before_time, hot_count);

	// Also check in-pass records so items projected this run are considered hot.
	if (static_cast<int>(hot_ids.size()) < hot_count)
	{
		std::vector<std::pair<std::string, std::time_t>> pass_hot;
		for (const auto& pr : play_records)
		{
			if (pr.item_type == "movie" && pr.aired_at < before_time) pass_hot.push_back({pr.item_id, pr.aired_at});
		}
		std::sort(pass_hot.begin(), pass_hot.end(), [](auto& a, auto& b)
		{
			return a.second > b.second; // newest first
		});
		for (const auto& ph : pass_hot)
		{
			hot_ids.insert(ph.first);
			if (static_cast<int>(hot_ids.size()) >= hot_count) break;
		}
	}

	if (hot_ids.empty()) return selectWeighted(block, rng);

	int total = 0;
	for (const auto& bc : block.content) if (!hot_ids.contains(bc.content_id)) total += std::max(1, bc.weight);
	if (total <= 0) return selectWeighted(block, rng); // all hot — fall back

	std::uniform_int_distribution<int> dist(0, total - 1);
	int r = dist(rng);
	for (int i = 0; i < n; ++i)
	{
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
	int n         = static_cast<int>(all.size());
	int hot_count = std::max(0, n * smart_pct / 100);
	if (hot_count == 0 || all.empty()) return all;

	auto hot_ids = content_.getHotEpisodeIds(channel_id, before_time, show_id, hot_count);

	// Also check in-pass records so items projected this run are considered hot.
	if (static_cast<int>(hot_ids.size()) < hot_count)
	{
		std::vector<std::pair<std::string, std::time_t>> pass_hot;
		for (const auto& pr : play_records)
		{
			if (pr.item_type == "episode" && pr.show_id == show_id && pr.aired_at < before_time) pass_hot.push_back({pr.item_id, pr.aired_at});
		}
		std::sort(pass_hot.begin(), pass_hot.end(), [](auto& a, auto& b)
		{
			return a.second > b.second; // newest first
		});
		for (const auto& ph : pass_hot)
		{
			hot_ids.insert(ph.first);
			if (static_cast<int>(hot_ids.size()) >= hot_count) break;
		}
	}

	if (hot_ids.empty()) return all;

	std::vector<Episode> filtered;
	filtered.reserve(all.size());
	for (const auto& e : all) if (!hot_ids.contains(e.episode_id)) filtered.push_back(e);

	return filtered.empty() ? all : filtered;
}

int RuleEngine::snapToGroupStart(const std::string& episode_id,
								 const std::vector<Episode>& eps)
{
	auto part1 = content_.findGroupPart1(episode_id);
	if (!part1) return -1;
	for (int i = 0; i < static_cast<int>(eps.size()); ++i) if (eps[i].episode_id == *part1) return i;
	return -1;
}

std::optional<Movie> RuleEngine::getMovie(const std::string& movie_id)
{
	return content_.getMovie(movie_id);
}

std::vector<std::pair<std::string, std::string>> RuleEngine::loadListItems(const std::string& content_type, const std::string& content_id)
{
	return content_.loadListItems(content_type, content_id);
}

std::string RuleEngine::getPlaylistMode(const std::string& playlist_id)
{
	return content_.getPlaylistMode(playlist_id);
}

std::vector<std::string> RuleEngine::getPlaylistShows(const std::string& playlist_id)
{
	return content_.getPlaylistShows(playlist_id);
}

std::vector<Episode> RuleEngine::getPlaylistShowEpisodes(const std::string& playlist_id,
														 const std::string& show_id)
{
	return content_.getPlaylistShowEpisodes(playlist_id, show_id);
}

std::optional<ScheduledItem> RuleEngine::episodeById(const std::string& episode_id)
{
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

std::string RuleEngine::showTitle(const std::string& show_id)
{
	return content_.showTitle(show_id);
}

// ── Block resolution ──────────────────────────────────────────────────────────

std::optional<Block> RuleEngine::resolveFromList(const std::vector<Block>& blocks,
												 std::time_t t,
												 const std::string& tz)
{
	auto tm     = toChannelTZ(t, tz);
	int day_bit = dayBit(tm);
	int cur_sec = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;

	const Block* best = nullptr;
	for (const auto& b : blocks)
	{
		if (!(b.day_mask & day_bit)) continue;
		int start_sec = parseTimeMins(b.start_time) * 60;
		// Align the block's nominal start to the next grid boundary.
		int eff_start_sec = start_sec;
		if (b.align_to_mins > 0)
		{
			int step      = b.align_to_mins * 60;
			int aligned   = ((start_sec + step - 1) / step) * step;
			eff_start_sec = aligned < 86400 ? aligned : 86400; // clamp — don't wrap past midnight
		}
		// early_start_secs expands the match window backward from the effective start.
		if (cur_sec < eff_start_sec - b.early_start_secs) continue;
		if (cur_sec > eff_start_sec + b.late_start_mins * 60 && b.start_scope == "block") continue;
		if (b.end_time.has_value())
		{
			if (cur_sec >= parseTimeMins(*b.end_time) * 60) continue;
		}
		// blocks are pre-sorted priority DESC; first match wins
		if (!best)
		{
			best = &b;
			break;
		}
	}
	if (!best) return std::nullopt;
	return *best;
}

std::string RuleEngine::channelTimezone(const std::string& channel_id)
{
	return blocks_.channelTimezone(channel_id);
}

std::string RuleEngine::channelAdvanceMode(const std::string& channel_id)
{
	return blocks_.channelAdvanceMode(channel_id);
}

std::optional<Block> RuleEngine::resolveBlock(const std::string& channel_id, std::time_t t)
{
	auto blocks = loadBlocks(channel_id);
	return resolveFromList(blocks, t, channelTimezone(channel_id));
}

// ── Item selection ────────────────────────────────────────────────────────────

static std::optional<ScheduledItem> itemFromShow(
	const std::string& channel_id,
	const std::string& block_id,
	const std::vector<Episode>& eps,
	int ep_pos,
	const std::string& show_title_str)
{
	if (eps.empty()) return std::nullopt;
	ep_pos         = ep_pos % static_cast<int>(eps.size());
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

static ScheduledItem movieItem(const Movie& m)
{
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
												  std::time_t before_time)
{
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
bool RuleEngine::hasRealHistory(const std::string& channel_id, const Block& block,
								const BlockContent& entry, std::time_t before_time,
								const std::vector<PlayRecord>& pass_records)
{
	if (std::any_of(pass_records.begin(), pass_records.end(),
					[&](const PlayRecord& pr) { return pr.show_id == entry.content_id; }))
		return true;
	bool global = (block.cursor_scope == CursorScope::Global);
	auto played = getPlayedEpisodes(entry.content_id, channel_id, entry.season_filter,
									before_time, global, entry.include_specials, entry.episode_order);
	return !played.empty();
}

// ── Show cursor: seed/advance/fetch, the single place that owns show playback ──

std::optional<ScheduledItem> RuleEngine::advanceShowCursor(
	const std::string& channel_id, const Block& block,
	const BlockContent& entry, std::time_t before_time,
	CursorState& state, const std::vector<PlayRecord>& pass_records,
	Xoshiro256& rng)
{
	const auto scope    = scopeStr(block);
	const auto scope_id = scopeId(block, channel_id);

	auto all = getEpisodes(entry.content_id, entry.season_filter,
						   entry.include_specials, entry.episode_order);
	if (all.empty()) return std::nullopt;

	// Free-random phase: every pick is independent, nothing persisted. Smart
	// advancement still applies its recency cooldown to the draw pool.
	auto freeRandomPick = [&]() -> std::optional<ScheduledItem>
	{
		auto pool = (block.advancement == Advancement::Smart && block.smart_pct > 0)
						? smartShufflePool(all, entry.content_id, channel_id, block.smart_pct, before_time, pass_records)
						: all;
		if (pool.empty()) pool = all;
		std::uniform_int_distribution<int> dist(0, static_cast<int>(pool.size()) - 1);
		int pos = dist(rng);
		if (block.snap_to_group_start)
		{
			int sn = snapToGroupStart(pool[pos].episode_id, pool);
			if (sn >= 0) pos = sn;
		}
		return itemFromShow(channel_id, block.block_id, pool, pos, showTitle(entry.content_id));
	};

	// Already caught up to the full catalog on some earlier call — stays free-random
	// forever from here; no need to keep re-checking real history.
	if (state.hasCursor("show_rerun", entry.content_id, scope, scope_id)) return freeRandomPick();

	// Fallback never tracks real history at all — always free-random.
	if (block.no_history_behavior == NoHistoryBehavior::FallbackAll)
	{
		state.setCursorPos("show_rerun", entry.content_id, scope, scope_id, 0, all[0].episode_id);
		return freeRandomPick();
	}

	// Normal, or Exclude now that pickNextContent has confirmed this show has real
	// history: resume right after whatever has actually aired so far, obeying the
	// block's cursor scope (channel/global play list). Recomputed fresh every call —
	// never cached — so this can't get ahead of whatever's actually airing the show
	// (typically a timeslot block). A show with no such source becomes its own
	// history this way, since its plays are recorded through the same play-history
	// path as everything else; a show that does have one just naturally tracks it.
	bool global = (block.cursor_scope == CursorScope::Global);
	auto played = getPlayedEpisodes(entry.content_id, channel_id, entry.season_filter,
									before_time, global, entry.include_specials, entry.episode_order);
	int aired = static_cast<int>(played.size()) + static_cast<int>(std::count_if(
		pass_records.begin(), pass_records.end(),
		[&](const PlayRecord& pr) { return pr.show_id == entry.content_id; }));

	if (aired >= static_cast<int>(all.size()))
	{
		state.setCursorPos("show_rerun", entry.content_id, scope, scope_id, 0, all[0].episode_id);
		return freeRandomPick();
	}
	return itemFromShow(channel_id, block.block_id, all, aired, showTitle(entry.content_id));
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

	const int n           = static_cast<int>(block.content.size());
	const int content_pos = state.getContentPosition(block.block_id) % n;
	const auto& cur_entry = block.content[content_pos];

	const bool is_rerun = isRerunMode(block);
	const bool is_first = !state.hasBlockPosition(block.block_id);

	// Only rerun shows and non-rerun Sequential shows track run counts.
	const bool has_runs = is_rerun ||
		(cur_entry.content_type == "show" && block.advancement == Advancement::Sequential);

	const int runs_left   = has_runs ? state.getRunsRemaining(block.block_id) : 0;
	const int consecutive = state.getConsecutiveCount(block.block_id) + 1;
	const bool limit_hit  = block.max_consecutive_episodes > 0 &&
		consecutive >= block.max_consecutive_episodes;
	const bool should_advance = !is_first && (!has_runs || runs_left == 0 || limit_hit);

	int sel = content_pos;

	if (should_advance)
	{
		if (is_rerun && block.no_history_behavior == NoHistoryBehavior::Exclude)
		{
			// Filter to shows with real history before drawing — never retried after
			// the fact, so a show with zero history simply isn't a candidate this call.
			std::vector<int> eligible;
			int total_w = 0;
			for (int i = 0; i < n; ++i)
			{
				if (limit_hit && i == content_pos) continue;
				const auto& c = block.content[i];
				if (c.content_type == "show" && !hasRealHistory(channel_id, block, c, before_time, pass_records)) continue;
				eligible.push_back(i);
				total_w += std::max(1, c.weight);
			}
			if (eligible.empty())
			{
				state.setBlockPosition(block.block_id, 0, 0, 0);
				return -1;
			}
			std::uniform_int_distribution<int> dist(0, total_w - 1);
			int r = dist(rng);
			sel   = eligible.back();
			for (int idx : eligible)
			{
				r -= std::max(1, block.content[idx].weight);
				if (r < 0)
				{
					sel = idx;
					break;
				}
			}
		}
		else if (block.advancement == Advancement::Sequential)
		{
			sel = (content_pos + 1) % n;
		}
		else if (block.advancement == Advancement::Smart && block.smart_pct > 0)
		{
			sel = selectWeightedSmartCooldown(block, channel_id, block.smart_pct,
											  before_time, pass_records, rng);
			if (limit_hit && sel == content_pos && n > 1) sel = selectWeightedExcluding(block, content_pos, rng);
		}
		else
		{
			sel = selectWeighted(block, rng);
			if (sel == content_pos && limit_hit && n > 1) sel = selectWeightedExcluding(block, content_pos, rng);
		}
	}

	const auto& sel_entry = block.content[sel];
	const bool same       = (sel == content_pos) && !is_first;

	// Episode-level cursor seed/advance is owned entirely by advanceShowCursor,
	// called from advanceAndGet once `sel` is settled — nothing to do here.

	// Update block state.
	const bool sel_has_runs = is_rerun ||
		(sel_entry.content_type == "show" && block.advancement == Advancement::Sequential);

	const int next_run = !sel_has_runs
							 ? 0
							 : is_rerun
							 ? std::max(1, sel_entry.run_count) - 1
							 : std::max(1, sel_entry.weight) - 1;

	if (!same)
	{
		state.setBlockPosition(block.block_id, sel, next_run, 0);
	}
	else
	{
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

	const int n           = static_cast<int>(block.content.size());
	const int content_pos = content_idx % n;
	const auto& entry     = block.content[content_pos];

	std::optional<ScheduledItem> item;

	// ── Show ─────────────────────────────────────────────────────────────────────
	if (entry.content_type == "show")
	{
		if (isRerunMode(block))
		{
			return advanceShowCursor(channel_id, block, entry, before_time, state, pass_records, rng);
		}
		else
		{
			// ── Non-rerun show (Sequential / Shuffle / SmartShuffle) ───────────────
			const std::string scope    = scopeStr(block);
			const std::string scope_id = scopeId(block, channel_id);

			auto all_eps = getEpisodes(entry.content_id, entry.season_filter,
									   entry.include_specials, entry.episode_order);
			if (all_eps.empty()) return std::nullopt;

			int pos = state.getCursorPos("show", entry.content_id, scope, scope_id);
			int next_pos;

			// For sequential mode: reconcile the integer cursor position against the
			// stored episode_id. If the episode list changed since the cursor was last
			// saved (e.g. a sync added episodes), the integer position may point to the
			// wrong episode. Find the stored episode_id and resume from there.
			if (block.advancement == Advancement::Sequential)
			{
				const std::string stored_ep_id =
					state.getCursorEpisodeId("show", entry.content_id, scope, scope_id);
				if (!stored_ep_id.empty())
				{
					int n        = static_cast<int>(all_eps.size());
					int safe_pos = pos % n;
					if (all_eps[safe_pos].episode_id != stored_ep_id)
					{
						// Episode list changed: search for the episode we meant to pick.
						for (int i = 0; i < n; ++i)
						{
							if (all_eps[i].episode_id == stored_ep_id)
							{
								pos = i;
								break;
							}
						}
						// If not found (episode removed), pos stays at safe_pos (best effort).
					}
				}
			}

			if (block.advancement == Advancement::Smart && block.smart_pct > 0)
			{
				auto eps = smartShufflePool(all_eps, entry.content_id, channel_id,
											block.smart_pct, before_time, state.playRecords());
				auto perm = shufflePermutation(entry.content_id + block.block_id,
											   static_cast<int>(eps.size()));
				item = itemFromShow(channel_id, block.block_id, eps,
									perm[pos % static_cast<int>(perm.size())],
									showTitle(entry.content_id));
				next_pos = pos + 1;
			}
			else if (block.advancement == Advancement::Shuffle)
			{
				int epoch = pos / static_cast<int>(all_eps.size());
				int idx   = pos % static_cast<int>(all_eps.size());
				auto perm = shufflePermutation(
					entry.content_id + block.block_id + std::to_string(epoch),
					static_cast<int>(all_eps.size()));
				item = itemFromShow(channel_id, block.block_id, all_eps, perm[idx],
									showTitle(entry.content_id));
				next_pos = pos + 1;
			}
			else
			{
				item = itemFromShow(channel_id, block.block_id, all_eps, pos,
									showTitle(entry.content_id));
				next_pos = (pos + 1) % static_cast<int>(all_eps.size());
			}
			std::string ep_id = all_eps[next_pos % static_cast<int>(all_eps.size())].episode_id;
			state.setCursorPos("show", entry.content_id, scope, scope_id, next_pos, ep_id);
			return item; // Block position managed by pickNextContent
		}
	}
	else if (entry.content_type == "movie")
	{
		// ── Movie ─────────────────────────────────────────────────────────────────
		auto m = getMovie(entry.content_id);
		if (!m) return std::nullopt;
		auto mi       = movieItem(*m);
		mi.channel_id = channel_id;
		mi.block_id   = block.block_id;
		item          = std::move(mi);
	}
	else if (entry.content_type == "episode")
	{
		// ── Single episode ────────────────────────────────────────────────────────
		auto ei = episodeById(entry.content_id);
		if (!ei) return std::nullopt;
		ei->channel_id = channel_id;
		ei->block_id   = block.block_id;
		item           = std::move(*ei);
	}
	else if (entry.content_type == "playlist" || entry.content_type == "filler_list")
	{
		if (entry.content_type == "playlist" && getPlaylistMode(entry.content_id) == "show_collection")
		{
			// ── show_collection ───────────────────────────────────────────────────
			auto shows = getPlaylistShows(entry.content_id);
			if (shows.empty()) return std::nullopt;
			int n_shows                = static_cast<int>(shows.size());
			const std::string scope    = scopeStr(block);
			const std::string scope_id = scopeId(block, channel_id);
			int show_idx               = state.getCursorPos("playlist", entry.content_id, scope, scope_id) % n_shows;
			const std::string& show_id = shows[show_idx];

			if (isRerunMode(block))
			{
				// A show_collection member has no BlockContent of its own — a synthetic
				// one (defaults matching what this path always used: no season filter,
				// no specials, season order) lets it share advanceShowCursor with the
				// regular "show" content-type path instead of duplicating its logic.
				BlockContent show_entry;
				show_entry.content_type = "show";
				show_entry.content_id   = show_id;
				item                    = advanceShowCursor(channel_id, block, show_entry, before_time, state, pass_records, rng);
			}
			else
			{
				// Non-rerun show_collection
				auto pl_eps = getPlaylistShowEpisodes(entry.content_id, show_id);
				if (!pl_eps.empty())
				{
					int pos = state.getCursorPos("show", show_id, scope, scope_id);
					int next_pos;
					if (block.advancement == Advancement::Smart && block.smart_pct > 0)
					{
						auto filt = smartShufflePool(pl_eps, show_id, channel_id,
													 block.smart_pct, before_time, state.playRecords());
						auto perm = shufflePermutation(show_id + block.block_id,
													   static_cast<int>(filt.size()));
						item = itemFromShow(channel_id, block.block_id, filt,
											perm[pos % static_cast<int>(perm.size())],
											showTitle(show_id));
						next_pos = pos + 1;
					}
					else if (block.advancement == Advancement::Shuffle)
					{
						int epoch = pos / static_cast<int>(pl_eps.size());
						int idx   = pos % static_cast<int>(pl_eps.size());
						auto perm = shufflePermutation(
							show_id + block.block_id + std::to_string(epoch),
							static_cast<int>(pl_eps.size()));
						item = itemFromShow(channel_id, block.block_id, pl_eps, perm[idx],
											showTitle(show_id));
						next_pos = pos + 1;
					}
					else
					{
						item = itemFromShow(channel_id, block.block_id, pl_eps, pos,
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
			if (n_shows == 1)
			{
				next_idx = 0;
			}
			else if (block.advancement == Advancement::Shuffle ||
				block.advancement == Advancement::Smart)
			{
				std::uniform_int_distribution<int> dist(0, n_shows - 1);
				next_idx = dist(rng);
			}
			else
			{
				next_idx = (show_idx + 1) % n_shows;
			}

			// No pre-seed needed for the next show — advanceShowCursor seeds lazily
			// on whatever call first actually selects it.
			state.setCursorPos("playlist", entry.content_id, scope, scope_id, next_idx);
		}
		else
		{
			// ── Flat playlist / filler_list ───────────────────────────────────────
			auto list_items = loadListItems(entry.content_type, entry.content_id);
			if (list_items.empty()) return std::nullopt;

			const std::string scope    = scopeStr(block);
			const std::string scope_id = scopeId(block, channel_id);
			int pos                    = state.getCursorPos(entry.content_type, entry.content_id, scope, scope_id)
				% static_cast<int>(list_items.size());
			const auto& [ptype, pid] = list_items[pos];

			if (ptype == "episode")
			{
				auto ei = episodeById(pid);
				if (!ei) return std::nullopt;
				ei->channel_id = channel_id;
				ei->block_id   = block.block_id;
				item           = std::move(*ei);
			}
			else
			{
				auto mi = getMovie(pid);
				if (!mi) return std::nullopt;
				auto m       = movieItem(*mi);
				m.channel_id = channel_id;
				m.block_id   = block.block_id;
				item         = std::move(m);
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
	std::time_t before_time,
	std::optional<std::time_t>& history_cached_before,
	std::unordered_map<std::string, int64_t>& history_cache,
	std::unordered_map<std::string, std::vector<FillerItem>>& items_cache)
{
	if (pool.empty()) return std::nullopt;
	int pool_size = static_cast<int>(pool.size());

	// Select which filler entry (filler list) to pull from.
	int entry_idx = 0;
	if (block.filler_selection == "round_robin")
	{
		std::string rr_key = "fl_rr:" + block.block_id;
		int& rr            = state.fillerPos(rr_key);
		entry_idx          = rr % pool_size;
		rr                 = (rr + 1) % pool_size;
	}
	else
	{
		if (block.filler_selection == "weighted")
		{
			int total = 0;
			for (const auto& e : pool) total += std::max(1, e.weight);
			std::uniform_int_distribution<int> dist(0, total - 1);
			int r = dist(rng);
			for (int i = 0; i < pool_size; ++i)
			{
				r -= std::max(1, pool[i].weight);
				if (r < 0)
				{
					entry_idx = i;
					break;
				}
			}
		}
		else
		{
			// random
			std::uniform_int_distribution<int> dist(0, pool_size - 1);
			entry_idx = dist(rng);
		}
	}

	// Load items from the selected filler source. If empty, try remaining pool entries in
	// order — a permanently empty source (unlinked list, no media) should not block filler
	// from other entries. The round-robin state is not rewound for the failed entry.
	struct FI
	{
		std::string type, id;
		int64_t dur = 0;
	};
	std::vector<FI> items;
	{
		int tried = 0;
		while (tried < pool_size)
		{
			const auto& src = pool[entry_idx];
			std::string key = src.content_type + ":" + src.content_id + ":"
				+ (src.season_filter.has_value() ? std::to_string(*src.season_filter) : "");
			auto cached = items_cache.find(key);
			if (cached == items_cache.end())
				cached = items_cache.emplace(key, content_.loadFillerItems(
												 src.content_type, src.content_id, src.season_filter)).first;
			for (const auto& fi : cached->second) items.push_back({fi.item_type, fi.item_id, fi.duration_ms});
			if (!items.empty()) break;
			entry_idx = (entry_idx + 1) % pool_size;
			++tried;
		}
	}
	if (items.empty()) return std::nullopt;
	const auto& fe = pool[entry_idx];

	// Determine item index within the list.
	int item_idx        = 0;
	std::string pos_key = "fl_pos:" + fe.content_type + ":" + fe.content_id + ":" + block.block_id;

	if (fe.advancement == "sized")
	{
		// Build eligible set: clips that fit within max_ms.
		std::vector<int> eligible;
		for (int i = 0; i < static_cast<int>(items.size()); ++i)
			if (items[i].dur > 0 && items[i].dur <= max_ms) eligible.push_back(i);
		if (eligible.empty()) return std::nullopt;

		// Prefer the least recently played eligible clip (never-played first).
		// The persisted-history query is expensive (full GROUP BY over
		// filler_play_history, which only grows over a channel's lifetime) and
		// — since this is forward projection, before_time is always at or past
		// "now" and thus already past every persisted row — its result is the
		// same for the whole pass. Compute it once, not on every single pick
		// (before_time increases nearly every call, so re-querying whenever it
		// advances is effectively every call — the bug this replaced).
		if (!history_cached_before)
		{
			history_cache         = content_.getLastPlayedMap(channel_id, before_time);
			history_cached_before = before_time;
		}
		auto last_played = history_cache;

		// Augment with in-pass picks: persisted history only reflects prior project()
		// calls, not clips already chosen earlier in this same gap/pass.
		for (const auto& pr : pass_records)
		{
			if (pr.channel_id != channel_id || pr.aired_at >= before_time) continue;
			auto it = last_played.find(pr.item_id);
			if (it == last_played.end() || pr.aired_at > it->second) last_played[pr.item_id] = pr.aired_at;
		}

		item_idx       = eligible[0];
		int64_t oldest = last_played.contains(items[eligible[0]].id)
							 ? last_played.at(items[eligible[0]].id)
							 : -1;
		for (int i = 1; i < static_cast<int>(eligible.size()); ++i)
		{
			int idx     = eligible[i];
			int64_t lat = last_played.contains(items[idx].id)
							  ? last_played.at(items[idx].id)
							  : -1;
			if (lat < oldest)
			{
				oldest   = lat;
				item_idx = idx;
			}
		}
		// "sized" is stateless — no cursor to advance.
	}
	else if (fe.advancement == "shuffle")
	{
		if (!state.hasFillerPos(pos_key)) state.fillerPos(pos_key) = static_cast<int>(rng() % static_cast<uint64_t>(items.size()));
		int& pos  = state.fillerPos(pos_key);
		int epoch = pos / static_cast<int>(items.size());
		int idx   = pos % static_cast<int>(items.size());
		auto perm = shufflePermutation(
			"fl:" + fe.content_type + ":" + fe.content_id + ":" + block.block_id + std::to_string(epoch),
			static_cast<int>(items.size()));
		item_idx = perm[idx];
		++pos;
	}
	else
	{
		// sequential
		if (!state.hasFillerPos(pos_key)) state.fillerPos(pos_key) = static_cast<int>(rng() % static_cast<uint64_t>(items.size()));
		int& pos = state.fillerPos(pos_key);
		item_idx = pos % static_cast<int>(items.size());
		pos      = (pos + 1) % static_cast<int>(items.size());
	}

	// smart_pct cooldown for shuffle/sequential: when the cursor-selected item is in the
	// hottest smart_pct% of the pool by recency, scan forward for a cooler alternative.
	// Sized advancement already applies LRU directly; this covers the other two modes.
	if (fe.advancement != "sized" && block.smart_pct > 0 && before_time > 0)
	{
		if (!history_cached_before)
		{
			history_cache         = content_.getLastPlayedMap(channel_id, before_time);
			history_cached_before = before_time;
		}
		auto lp = history_cache;

		// Augment last-played map with in-pass records.
		for (const auto& pr : pass_records)
		{
			if (pr.channel_id == channel_id && pr.aired_at < before_time)
			{
				auto it = lp.find(pr.item_id);
				if (it == lp.end() || pr.aired_at > it->second) lp[pr.item_id] = pr.aired_at;
			}
		}

		int hot_n = std::max(0, static_cast<int>(items.size()) * block.smart_pct / 100);
		if (hot_n > 0 && !lp.empty())
		{
			std::vector<int64_t> ts;
			ts.reserve(items.size());
			for (const auto& fitem : items)
			{
				auto it = lp.find(fitem.id);
				ts.push_back(it != lp.end() ? it->second : -1);
			}
			auto sorted_ts = ts;
			std::sort(sorted_ts.rbegin(), sorted_ts.rend());
			int64_t thresh = sorted_ts[hot_n - 1];
			if (thresh >= 0 && ts[item_idx] >= thresh)
			{
				int m = static_cast<int>(items.size());
				for (int d = 1; d < m; ++d)
				{
					int c = (item_idx + d) % m;
					if (ts[c] < thresh || ts[c] < 0)
					{
						item_idx = c;
						break;
					}
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
	if (content_type == "episode") return episodeById(content_id);
	if (content_type == "movie")
	{
		auto m = getMovie(content_id);
		if (!m) return std::nullopt;
		return movieItem(*m);
	}
	if (content_type == "show")
	{
		auto eps = getEpisodes(content_id, std::nullopt, true);
		if (eps.empty()) return std::nullopt;
		return itemFromShow(channel_id, "", eps,
							position % static_cast<int>(eps.size()),
							showTitle(content_id));
	}
	if (content_type == "playlist")
	{
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
	if (content_type == "show") pos = state.getCursorPos("show", content_id, "block", scope_id);
	else if (content_type == "playlist") pos = state.getCursorPos("playlist", content_id, "block", scope_id);

	return pickFromSource(channel_id, content_type, content_id, pos);
}

void RuleEngine::advanceBumperCursor(
	const std::string& content_type,
	const std::string& content_id,
	const std::string& scope_id,
	CursorState& state)
{
	if (content_type == "episode") return; // single-episode bumper — no cursor

	if (content_type == "show")
	{
		auto eps = getEpisodes(content_id, std::nullopt, true);
		if (!eps.empty())
		{
			int pos  = state.getCursorPos("show", content_id, "block", scope_id);
			int next = (pos + 1) % static_cast<int>(eps.size());
			state.setCursorPos("show", content_id, "block", scope_id,
							   next, eps[next].episode_id);
		}
	}
	else if (content_type == "playlist")
	{
		int n = content_.getPlaylistItemCount(content_id);
		if (n > 0)
		{
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

	auto& item               = *item_opt;
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

// ── Scheduling core ───────────────────────────────────────────────────────────
//
//  scheduleBlockStep — schedules exactly one program for `block`, bounded by its
//                      own precomputed window. Returns true if that exhausted the
//                      occurrence (program_count hit, or nothing left to schedule).
//
//  projectWeek       — dispatch loop for one week. Repeatedly resolves the active
//                      window and calls scheduleBlockStep; fills gaps with channel
//                      filler. See resolveActiveWindow/scheduleBlockStep doc above.
//
//  project           — outer loop: one fresh block+window copy per week, calls
//                      projectWeek per week.

// Applies `apply` to every schedule_windows entry of `block` sharing w's occurrence —
// prog_count/intro_played/exhausted describe the occurrence as a whole (see BlockWindow),
// so every piece cut from the same occurrence must stay in sync.
template <typename F>
static void syncOccurrence(Block& block, const BlockWindow& w, F apply)
{
	for (auto& sib : block.schedule_windows) if (sib.occurrence_start == w.occurrence_start) apply(sib);
}

bool RuleEngine::scheduleBlockStep(
	const ProjectContext& ctx,
	Block& block,
	BlockWindow& w,
	std::time_t window_end,
	ProjectPassState& pass)
{
	if (block.block_type == BlockType::Timeslot)
	{
		bool done = scheduleTimeslotBlock(ctx, block, window_end, pass);
		if (done) w.exhausted = true; // resolveActiveWindow checks w, not this return value
		return done;
	}

	// Content-entry/episode cursor seeding is owned entirely by pickNextContent
	// (called below) — no separate block-level seed step here.

	if (!w.intro_played)
	{
		if (!block.intro_content_id.empty())
			scheduleBumperItem(ctx.channel_id, block.block_id,
							   block.intro_content_type, block.intro_content_id,
							   block.block_id + ":intro", ctx.result, pass.t, ctx.state);
		syncOccurrence(block, w, [](BlockWindow& sib) { sib.intro_played = true; });
	}

	// Episode-start alignment: pad up to the next align_to_mins boundary with real
	// filler content *before* picking what plays here — this is the item that would
	// otherwise start off-grid, not just whatever comes after it. Runs before
	// selection (not after the previous push) specifically so it also covers the
	// first item of a window, where there is no previous push to react to (intro,
	// or nothing, can easily leave pass.t mid-interval).
	if (block.inter_filler && block.start_scope == "episode" && block.align_to_mins > 0)
	{
		const auto& pool = block.filler_entries.empty()
							   ? ctx.channel_filler
							   : block.filler_entries;
		
		std::time_t step   = static_cast<std::time_t>(block.align_to_mins) * 60;
		std::time_t prev_b = (pass.t / step) * step;
		std::time_t next_b = prev_b + step;
		bool in_early      = (next_b - pass.t) <= static_cast<std::time_t>(block.early_start_secs);
		bool in_late       = (pass.t - prev_b) <= static_cast<std::time_t>(block.late_start_mins) * 60;
		
		if (!in_early && !in_late)
		{
			if (!pool.empty())
			{
				const std::time_t fill_target   = next_b;
				const std::time_t late_boundary = next_b
					+ static_cast<std::time_t>(block.late_start_mins) * 60;
				while (pass.t < fill_target && pass.t < window_end)
				{
					int64_t rem_ms = (fill_target - pass.t) * 1000;
					int64_t max_ms = (late_boundary - pass.t) * 1000;
					auto fi        = pickFillerSim(ctx.channel_id, block, pool, rem_ms, ctx.state, ctx.rng, pass.filler_records, pass.t, pass.filler_history_cached_before, pass.filler_history_cache, pass.filler_items_cache);
					if (!fi || fi->duration_ms <= 0 || fi->duration_ms > max_ms) break;
					fi->wall_clock_start_ms = static_cast<int64_t>(pass.t) * 1000;
					fi->wall_clock_end_ms   = fi->wall_clock_start_ms + fi->duration_ms;
					fi->cursor_json         = "{}";
					fi->channel_id          = ctx.channel_id;
					fi->block_id            = block.block_id;
					const int64_t fi_dur    = fi->duration_ms;
					pass.filler_records.push_back({ctx.channel_id, fi->item_type, fi->item_id, fi->show_id, block.block_id, pass.t});
					ctx.result.push_back(std::move(*fi));
					pass.t          += fi_dur / 1000;
					std::time_t pb2 = (pass.t / step) * step, nb2 = pb2 + step;
					if ((nb2 - pass.t) <= static_cast<std::time_t>(block.early_start_secs) ||
						(pass.t - pb2) <= static_cast<std::time_t>(block.late_start_mins) * 60)
						break;
				}
			}
			else
				pass.t = next_b;
		}
	}

	const int64_t remaining_ms = static_cast<int64_t>(window_end - pass.t) * 1000;
	if (remaining_ms <= 0)
	{
		w.exhausted = true;
		return true;
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
	auto item_opt    = (sel >= 0)
						   ? advanceAndGet(ctx.channel_id, block, sel, pass.t, ctx.state, pass.play_records, ctx.rng)
						   : std::nullopt;

	// Doesn't fit what's left of the window — release the cursor advance (so this
	// episode is still next up next time) and fall through to sized filler instead
	// of overrunning into whatever comes after.
	if (item_opt && item_opt->duration_ms > 0 && item_opt->duration_ms > remaining_ms)
	{
		ctx.state = snap;
		item_opt.reset();
	}

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
	if (!item_opt)
	{
		const auto& pool = block.filler_entries.empty()
							   ? ctx.channel_filler
							   : block.filler_entries;
		if (!pool.empty())
			if (auto fi = pickFillerSim(ctx.channel_id, block, pool, remaining_ms, ctx.state, ctx.rng, pass.filler_records, pass.t, pass.filler_history_cached_before, pass.filler_history_cache, pass.filler_items_cache);
				fi && fi->duration_ms > 0 && fi->duration_ms <= remaining_ms)
			{
				item_opt           = std::move(fi);
				is_fallback_filler = true;
			}
	}
	if (!item_opt)
	{
		// Nothing available that fits this piece's remaining time — this piece is
		// done (not the whole occurrence: a later piece past a preemption cut gets
		// its own fresh budget). No arbitrary time nudge; the caller re-resolves
		// immediately, which may pick a different block for the rest of this window.
		w.exhausted = true;
		return true;
	}

	auto item = std::move(*item_opt);
	if (item.duration_ms <= 0)
	{
		// Corrupt data, not a real exhaustion — nudge past it and let the caller retry;
		// the content cursor already advanced past this item so a retry sees the next one.
		std::cout << "[epg] WARNING: item duration_ms=0 id=" << item.item_id
			<< " type=" << item.item_type
			<< " block=" << block.block_id << " — skipping\n";
		pass.t = std::min(pass.t + 60, window_end);
		return false;
	}

	const int64_t dur_ms = item.duration_ms;

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

	if (!is_fallback_filler)
	{
		pass.play_records.push_back({ctx.channel_id, ph_type, ph_id, ph_show, block.block_id, ph_at});
		ctx.state.addPlayRecord(ctx.channel_id, ph_type, ph_id, ph_show, block.block_id, ph_at);

		// Cull pass.play_records based on rerun_min_time_mins.
		int effective_min = block.rerun_min_time_mins > 0 ? block.rerun_min_time_mins : ctx.rerun_min_time_mins;
		if (effective_min > 0)
		{
			std::time_t threshold = pass.t - static_cast<std::time_t>(effective_min) * 60;
			pass.play_records.erase(
				std::remove_if(pass.play_records.begin(), pass.play_records.end(),
							   [&](const PlayRecord& pr) { return pr.aired_at < threshold; }),
				pass.play_records.end());
		}
	}
	else
	{
		pass.filler_records.push_back({ctx.channel_id, ph_type, ph_id, ph_show, block.block_id, ph_at});
	}

	if (!is_fallback_filler && !ctx.between_bumpers.empty())
	{
		++pass.channel_prog_count;
		for (const auto& bumper : ctx.between_bumpers)
		{
			if (bumper.every_n > 0 && pass.channel_prog_count % bumper.every_n == 0)
			{
				scheduleBumperItem(ctx.channel_id, block.block_id, bumper.ct, bumper.cid,
								   ctx.channel_id + ":b" + std::to_string(bumper.id),
								   ctx.result, pass.t, ctx.state);
				break;
			}
		}
	}

	bool prog_limit_hit = false;
	if (!is_fallback_filler && block.program_count > 0)
	{
		++w.prog_count;
		prog_limit_hit = w.prog_count >= block.program_count;
		// prog_count is a shared occurrence-wide budget, not per-piece — a later
		// piece past a preemption cut must see how much of it earlier pieces used.
		syncOccurrence(block, w, [&](BlockWindow& sib) { sib.prog_count = w.prog_count; });
	}

	if (prog_limit_hit)
	{
		// Outro plays within this occurrence's own window (like the intro, no separate
		// grid-alignment step) and is the true end of the occurrence — mark every sibling
		// piece exhausted too so a later piece never re-triggers it.
		if (!block.outro_content_id.empty())
			scheduleBumperItem(ctx.channel_id, block.block_id,
							   block.outro_content_type, block.outro_content_id,
							   block.block_id + ":outro", ctx.result, pass.t, ctx.state);
		syncOccurrence(block, w, [](BlockWindow& sib) { sib.exhausted = true; });
		if (epgDebug())
			std::cout << "[epg] exhausted t=" << pass.t
				<< " block=" << block.block_id.substr(0, 8)
				<< " prog_count=" << w.prog_count << '\n';
		return true;
	}
	return false;
}

// ─────────────────────────────────────────────────────────────────────────────

std::optional<RuleEngine::ActiveWindow> RuleEngine::resolveActiveWindow(
	std::vector<Block>& week_blocks, std::time_t week_start, std::time_t t)
{
	const int64_t rel = t - week_start;
	// week_blocks is sorted priority DESC, start_time DESC (BlockRepository::loadBlocks),
	// so the first covering, unexhausted window is the highest-priority owner of `t`.
	for (auto& b : week_blocks)
	{
		for (auto& w : b.schedule_windows)
		{
			if (w.exhausted) continue;
			if (rel >= w.window_start && rel < w.window_end) return ActiveWindow{&b, &w};
		}
	}
	return std::nullopt;
}

void RuleEngine::projectWeek(
	const ProjectContext& ctx,
	std::vector<Block>& week_blocks,
	std::time_t week_start,
	std::time_t week_end,
	ProjectPassState& pass)
{
	pass.transition_counts.clear();
	pass.last_show_id.clear();
	pass.prev_block_id.clear();

	while (pass.t < week_end)
	{
		auto active = resolveActiveWindow(week_blocks, week_start, pass.t);

		if (!active)
		{
			// Jump to the nearest upcoming window start (across every block) to avoid
			// crawling minute by minute through a stretch nothing covers.
			std::time_t jump = week_end;
			for (const auto& b : week_blocks)
				for (const auto& w : b.schedule_windows)
				{
					if (w.exhausted) continue;
					std::time_t cand = week_start + w.window_start;
					if (cand > pass.t && cand < jump) jump = cand;
				}
			// No window covers this stretch — materialize channel-level filler into the
			// gap via the same deterministic path as block-internal filler (rotation,
			// smart_pct cooldown), instead of leaving it unscheduled for the live "/now"
			// path to fill ad hoc with an ungoverned random pick.
			fillToTime(ctx, ctx.gap_block, jump, pass);
			pass.prev_block_id.clear();
			continue;
		}

		Block& block   = *active->block;
		BlockWindow& w = *active->window;

		if (block.block_id != pass.prev_block_id) pass.transition_counts[block.block_id] = 0;
		pass.prev_block_id = block.block_id;

		const std::time_t window_end_ts = week_start + w.window_end;
		scheduleBlockStep(ctx, block, w, window_end_ts, pass);
		// pass.t advanced (or w marked exhausted) by scheduleBlockStep; loop re-resolves.
	}
}

// ── Forward projection ────────────────────────────────────────────────────────

std::vector<ScheduledItem> RuleEngine::project(const std::string& channel_id,
											   std::time_t start, int horizon_hours,
											   CursorState& state,
											   Xoshiro256& rng,
											   std::map<std::time_t, std::string>* anchors_out,
											   std::vector<PlayRecord>* play_records_out,
											   std::vector<PlayRecord>* filler_records_out)
{
	std::vector<ScheduledItem> result;
	auto blocks = loadBlocks(channel_id);

	scheduleBlockWindows(blocks);

	if (epgDebug())
		std::cout << "[epg] project() channel=" << channel_id
			<< " start=" << start << " hours=" << horizon_hours
			<< " blocks=" << blocks.size() << '\n';

	if (blocks.empty())
	{
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
		while (cfq.executeStep())
		{
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
			between_bumpers.push_back({
				bq.getColumn(0).getInt(),
				bq.getColumn(1).getString(),
				bq.getColumn(2).getString(),
				bq.getColumn(3).getInt()
			});
	}

	int rerun_min                    = 0;
	std::string gap_filler_selection = "round_robin";
	{
		SQLite::Statement sq(db_.get(),
							 "SELECT rerun_min_time_mins, default_filler_selection FROM channel WHERE channel_id = ?");
		sq.bind(1, channel_id);
		if (sq.executeStep())
		{
			rerun_min            = sq.getColumn(0).getInt();
			gap_filler_selection = sq.getColumn(1).getString();
		}
	}

	// default block object for filling gaps
	Block gap_block;
	gap_block.filler_selection = gap_filler_selection;

	ProjectContext ctx{
		channel_id, blocks, channel_filler, between_bumpers,
		tz, start, rerun_min, result, state, rng, anchors_out, gap_block
	};

	ProjectPassState pass;
	pass.t                  = start;
	pass.anchor_next_monday = [&]() -> std::time_t
	{
		std::time_t d   = start / 86400;
		std::time_t dow = (d + 3) % 7; // 0 = Mon
		return (d - dow + 7) * 86400;  // always the coming Monday, never start itself
	}();

	const std::time_t t_end = start + static_cast<std::time_t>(horizon_hours) * 3600;

	while (pass.t < t_end)
	{
		auto tm_t             = toChannelTZ(pass.t, tz);
		int c_sec             = tm_t.tm_hour * 3600 + tm_t.tm_min * 60 + tm_t.tm_sec;
		std::time_t day_start = pass.t - static_cast<std::time_t>(c_sec);

		// Walk back to this week's Monday 00:00 (channel tz), one calendar day at a
		// time via toChannelTZ — never raw 86400 arithmetic, so a DST transition
		// somewhere in the week can't skew where "Monday" lands.
		std::time_t week_start = day_start;
		while (toChannelTZ(week_start, tz).tm_wday != 1)
		{
			std::time_t approx = week_start - 43200; // always lands in the previous calendar day
			auto tm_a          = toChannelTZ(approx, tz);
			int a_sec          = tm_a.tm_hour * 3600 + tm_a.tm_min * 60 + tm_a.tm_sec;
			week_start         = approx - static_cast<std::time_t>(a_sec);
		}

		// Next Monday 00:00: 8 days past week_start, minus half a day, always crosses
		// it safely regardless of DST.
		std::time_t w_approx = week_start + 8 * 86400 - 43200;
		auto tm_we           = toChannelTZ(w_approx, tz);
		int we_sec           = tm_we.tm_hour * 3600 + tm_we.tm_min * 60 + tm_we.tm_sec;
		std::time_t week_end = std::min(w_approx - static_cast<std::time_t>(we_sec), t_end);

		// Fresh copy of the pristine (already-windowed) block template each week —
		// BlockWindow::prog_count/intro_played/exhausted reset for free since this
		// copy is discarded at week's end and next week starts from `blocks` again.
		std::vector<Block> week_blocks = blocks;
		projectWeek(ctx, week_blocks, week_start, week_end, pass);

		// Guard: if projectWeek left pass.t short (no active windows, gap exhausted),
		// advance to week_end to prevent re-entering the same week.
		if (pass.t < week_end) pass.t = week_end;
	}

	if (epgDebug() || result.empty())
		std::cout << "[epg] project() channel=" << channel_id
			<< " => " << result.size() << " items"
			<< (result.empty() ? " (EMPTY — no content scheduled)" : "") << '\n';

	if (play_records_out) *play_records_out = std::move(pass.play_records);
	if (filler_records_out) *filler_records_out = std::move(pass.filler_records);
	return result;
}

// Split our blocks into the actual scheduled blocks after priority cuts.
void RuleEngine::scheduleBlockWindows(std::vector<Block>& blocks)
{
	static const int kDayBits[7]      = {1, 2, 4, 8, 16, 32, 64}; // Sun..Sat
	static const int kMondayOffset[7] = {6, 0, 1, 2, 3, 4, 5};    // Mon-indexed offset per bit above

	constexpr int64_t kWeekSecs = 7 * 86400;

	auto preempts = [](const Block& hp, const Block& block)
	{
		if (hp.priority != block.priority) return hp.priority > block.priority;
		return parseTimeMins(hp.start_time) > parseTimeMins(block.start_time);
	};

	for (size_t i = 0; i < blocks.size(); ++i)
	{
		Block& block      = blocks[i];
		int64_t start_sec = parseTimeMins(block.start_time) * 60;

		// Raw per-active-day occurrences, placed on the week timeline.
		std::vector<std::pair<int64_t, int64_t>> raw;
		for (int d = 0; d < 7; ++d)
		{
			if (!(block.day_mask & kDayBits[d])) continue;
			int64_t base = static_cast<int64_t>(kMondayOffset[d]) * 86400;
			int64_t s    = base + start_sec - block.early_start_secs;
			int64_t e;
			if (block.end_time.has_value())
			{
				int64_t end_sec = parseTimeMins(*block.end_time) * 60;
				e               = base + (end_sec <= start_sec ? end_sec + 86400 : end_sec);
			}
			else
			{
				e = base + 86400; // runs to day end
			}
			e = std::min(e, kWeekSecs); // Monday 00:00 is a hard boundary
			if (e > s) raw.emplace_back(s, e);
		}
		if (raw.empty()) continue;

		// Merge touching/overlapping occurrences (e.g. a 24/7 block's 7 daily spans
		// collapse into one [0, 604800) window).
		std::sort(raw.begin(), raw.end());
		std::vector<std::pair<int64_t, int64_t>> merged;
		for (auto& span : raw)
		{
			if (!merged.empty() && span.first <= merged.back().second) merged.back().second = std::max(merged.back().second, span.second);
			else merged.push_back(span);
		}

		// Preemptor start times, placed on the same week timeline. adding late start seconds for overflow and filler
		std::vector<std::pair<int64_t, int64_t>> cuts;
		for (size_t j = 0; j < i; ++j)
		{
			const Block& hp = blocks[j];
			if (!preempts(hp, block)) continue;
			int64_t hp_start_sec = parseTimeMins(hp.start_time) * 60;
			for (int d = 0; d < 7; ++d)
			{
				if (!(hp.day_mask & kDayBits[d])) continue;
				int64_t cut_start = static_cast<int64_t>(kMondayOffset[d]) * 86400 + hp_start_sec;
				cuts.push_back({cut_start, cut_start + hp.late_start_mins * 60});
			}
		}
		std::sort(cuts.begin(), cuts.end());
		cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

		// Split each merged span at whichever cuts fall strictly inside it. All pieces
		// cut from the same span share span.first as occurrence_start, so a preemption
		// split doesn't reset program_count exhaustion — only a different occurrence
		// (different day) does.
		for (auto& span : merged)
		{
			int64_t seg_start = span.first;
			for (std::pair<int64_t, int64_t> cut : cuts)
			{
				if (cut.second <= seg_start) continue;
				if (cut.second >= span.second) break;
				block.schedule_windows.push_back({seg_start, cut.second, span.first});
				seg_start = cut.first;
			}
			block.schedule_windows.push_back({seg_start, span.second, span.first});
		}
	}
}

// ── Timeslot content selection ────────────────────────────────────────────────
// A timeslot slot's programming lives in TimeslotSlot::queue, not block.content
// (which is always empty for a timeslot block) — pickNextContent/advanceAndGet
// don't apply here.

// Formats t's calendar date (in tz) as "YYYY-MM-DD" — ISO 8601 dates compare
// correctly as plain strings, so premiere_date gating needs no date parsing.
static std::string dateStringForCompare(std::time_t t, const std::string& tz)
{
	auto tm = toChannelTZ(t, tz);
	char buf[11];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
	return buf;
}

std::optional<ScheduledItem> RuleEngine::pickTimeslotItem(
	const std::string& channel_id,
	const Block& block,
	const TimeslotSlot& slot,
	SlotCursor& sc,
	std::time_t at,
	const std::string& tz)
{
	const int n = static_cast<int>(slot.queue.size());
	if (n == 0) return std::nullopt;

	const std::string today = dateStringForCompare(at, tz);
	auto premiered          = [&](const TimeslotQueueEntry& e)
	{
		return e.premiere_date.empty() || today >= e.premiere_date;
	};

	int idx                         = sc.queue_pos % n;
	const TimeslotQueueEntry* entry = &slot.queue[idx];
	// False while serving a stand-in for a not-yet-premiered entry — the queue
	// must not advance past it, so it's retried (and re-gated) on the next call.
	bool advancing_queue = true;

	if (!premiered(*entry))
	{
		if (entry->pre_premiere_behavior == "replay_previous" && n > 1)
		{
			entry           = &slot.queue[(idx - 1 + n) % n];
			advancing_queue = false;
		}
		else if (entry->pre_premiere_behavior == "skip")
		{
			int next_idx = (idx + 1) % n;
			if (!premiered(slot.queue[next_idx])) return std::nullopt; // not ready either — fall to filler
			idx   = next_idx;
			entry = &slot.queue[idx];
		}
		else
		{
			return std::nullopt; // "filler" behavior, or replay_previous with no prior entry
		}
	}

	if (entry->content_type == "movie")
	{
		auto m = getMovie(entry->content_id);
		if (!m) return std::nullopt;
		auto item       = movieItem(*m);
		item.channel_id = channel_id;
		item.block_id   = block.block_id;
		if (advancing_queue)
		{
			sc.queue_pos   = (idx + 1) % n;
			sc.episode_pos = 0;
		}
		return item;
	}

	// "show" — sequential episode order, advancing to the next queue entry
	// once this show's episode list is exhausted.
	auto eps = getEpisodes(entry->content_id, std::nullopt, false, "season");
	if (eps.empty()) return std::nullopt;
	int ep_pos = sc.episode_pos % static_cast<int>(eps.size());
	auto item  = itemFromShow(channel_id, block.block_id, eps, ep_pos, showTitle(entry->content_id));
	if (!item) return std::nullopt;

	if (advancing_queue)
	{
		if (ep_pos + 1 >= static_cast<int>(eps.size()))
		{
			sc.queue_pos   = (idx + 1) % n;
			sc.episode_pos = 0;
		}
		else { sc.episode_pos = ep_pos + 1; }
	}
	return item;
}

// ── Timeslot block scheduling ─────────────────────────────────────────────────

void RuleEngine::fillToTime(const ProjectContext& ctx,
							const Block& block,
							std::time_t target,
							ProjectPassState& pass)
{
	if (pass.t >= target)
	{
		pass.t = target;
		return;
	}
	const auto& pool = block.filler_entries.empty()
						   ? ctx.channel_filler
						   : block.filler_entries;
	if (pool.empty())
	{
		pass.t = target;
		return;
	}
	int guard = 0;
	while (pass.t < target && guard++ < 2000)
	{
		int64_t max_ms = (target - pass.t) * 1000;
		auto fi        = pickFillerSim(ctx.channel_id, block, pool, max_ms, ctx.state, ctx.rng, pass.filler_records, pass.t, pass.filler_history_cached_before, pass.filler_history_cache, pass.filler_items_cache);
		if (!fi || fi->duration_ms <= 0 || fi->duration_ms > max_ms) break;
		fi->wall_clock_start_ms = static_cast<int64_t>(pass.t) * 1000;
		fi->wall_clock_end_ms   = fi->wall_clock_start_ms + fi->duration_ms;
		fi->cursor_json         = "{}";
		fi->channel_id          = ctx.channel_id;
		fi->block_id            = block.block_id;
		int64_t dur             = fi->duration_ms;
		pass.filler_records.push_back({ctx.channel_id, fi->item_type, fi->item_id, fi->show_id, block.block_id, pass.t});
		ctx.result.push_back(std::move(*fi));
		pass.t += dur / 1000;
	}
	if (pass.t < target) pass.t = target;
}

bool RuleEngine::scheduleTimeslotBlock(
	const ProjectContext& ctx,
	const Block& block,
	std::time_t window_end,
	ProjectPassState& pass)
{
	// Anchored to whatever calendar day pass.t falls on right now — correct because
	// the dispatcher only ever calls this right as the occurrence's window becomes
	// active, so pass.t is still on the occurrence's own day.
	auto tm_t             = toChannelTZ(pass.t, ctx.tz);
	int c_sec             = tm_t.tm_hour * 3600 + tm_t.tm_min * 60 + tm_t.tm_sec;
	std::time_t day_start = pass.t - static_cast<std::time_t>(c_sec);

	std::time_t block_nominal_start = day_start
		+ static_cast<std::time_t>(parseTimeMins(block.start_time)) * 60;

	std::time_t block_end_ts = std::numeric_limits<std::time_t>::max();
	if (block.end_time.has_value())
	{
		int end_secs     = parseTimeMins(*block.end_time) * 60;
		int start_secs   = parseTimeMins(block.start_time) * 60;
		std::time_t base = (end_secs < start_secs) ? day_start + 86400 : day_start;
		block_end_ts     = base + static_cast<std::time_t>(end_secs);
	}
	else if (!block.slots.empty())
	{
		// No explicit end_time: derive implicit end from the last slot's absolute end.
		// This prevents the trailing fillToTime from running to t_end after slots complete.
		const auto& last = block.slots.back();
		block_end_ts     = block_nominal_start
			+ static_cast<std::time_t>(last.slot_offset_mins + last.slot_duration_mins) * 60;
	}

	// window_end is already grace-adjusted (scheduleBlockWindows folds a preemptor's
	// late_start_mins into the cut) — no separate soft/hard distinction needed here.
	const std::time_t loop_end = std::min(window_end, block_end_ts);

	bool any_slot_ran = false;

	for (const auto& slot : block.slots)
	{
		std::time_t slot_start = block_nominal_start
			+ static_cast<std::time_t>(slot.slot_offset_mins) * 60;
		std::time_t slot_end = slot_start
			+ static_cast<std::time_t>(slot.slot_duration_mins) * 60;
		slot_end = std::min(slot_end, loop_end);

		if (slot_end <= pass.t) continue;  // entirely in the past
		if (slot_start >= loop_end) break; // beyond block window

		// This slot is in the current window; mark the block as active for this pass.
		any_slot_ran = true;

		// Fill gap before this slot with bumpers/filler.
		if (pass.t < slot_start) fillToTime(ctx, block, slot_start, pass);

		// Late-start check: skip slot if we've blown past its entry window.
		std::time_t lwe = slot_start
			+ static_cast<std::time_t>(slot.late_start_mins) * 60;
		if (pass.t > lwe)
		{
			pass.t = slot_end;
			continue;
		}

		// Slot entry alignment.
		if (slot.start_scope != "episode" && slot.align_to_mins > 0)
		{
			std::time_t step  = static_cast<std::time_t>(slot.align_to_mins) * 60;
			std::time_t c_sec = pass.t - day_start;
			std::time_t ls    = ((c_sec + step - 1) / step) * step;
			std::time_t eff   = day_start + ls;
			if (eff > pass.t && eff < slot_end) pass.t = eff;
		}

		// Schedule content — resolved from this slot's own queue, not block.content.
		auto sc       = ctx.state.getSlotCursor(slot.slot_id);
		auto item_opt = pickTimeslotItem(ctx.channel_id, block, slot, sc, pass.t, ctx.tz);
		if (item_opt)
		{
			int64_t dur_ms = item_opt->duration_ms;
			if (slot.overflow == SlotOverflow::Cutoff)
			{
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
			if (effective_min > 0)
			{
				std::time_t threshold = pass.t - static_cast<std::time_t>(effective_min) * 60;
				pass.play_records.erase(
					std::remove_if(pass.play_records.begin(), pass.play_records.end(),
								   [&](const PlayRecord& pr) { return pr.aired_at < threshold; }),
					pass.play_records.end());
			}
			ctx.result.push_back(std::move(*item_opt));
			pass.t += dur_ms / 1000;

			// pickTimeslotItem() already advanced sc in place (queue_pos and/or
			// episode_pos) — just persist it.
			ctx.state.setSlotCursor(slot.slot_id, sc.queue_pos, sc.episode_pos);
		}
		else
		{
			if (slot.overflow == SlotOverflow::Cutoff) pass.t = slot_end;
		}

		// After content: fill remainder of slot with filler (cutoff mode),
		// or leave pass.t where it is (finish mode — episode ran over).
		if (slot.overflow == SlotOverflow::Cutoff && pass.t < slot_end) fillToTime(ctx, block, slot_end, pass);
	}

	// Fill time between the last slot and block/window end — only when the block
	// actually ran content this pass. If all slots were already in the past we signal
	// exhaustion instead; the caller marks this window exhausted so it isn't retried.
	if (any_slot_ran)
	{
		if (pass.t < loop_end) fillToTime(ctx, block, loop_end, pass);
		// Guard: if the filler pool was empty and couldn't advance pass.t, force it to
		// loop_end so the dispatcher moves on rather than spinning on this block.
		if (pass.t < loop_end) pass.t = loop_end;
		return false;
	}

	// loop_end may be preemption-narrowed rather than the block's real end
	// (block_end_ts) — check the latter so a preempted-before-next-slot pass
	// doesn't get marked exhausted and lose its remaining slots for the day.
	bool more_slots_today = false;
	for (const auto& slot : block.slots)
	{
		std::time_t s_start = block_nominal_start
			+ static_cast<std::time_t>(slot.slot_offset_mins) * 60;
		std::time_t s_end = std::min(
			s_start + static_cast<std::time_t>(slot.slot_duration_mins) * 60,
			block_end_ts);
		if (s_end > pass.t)
		{
			more_slots_today = true;
			break;
		}
	}
	if (more_slots_today)
	{
		// Advance past the preemption boundary so the dispatcher re-resolves once it clears.
		if (pass.t < loop_end) fillToTime(ctx, block, loop_end, pass);
		if (pass.t < loop_end) pass.t = loop_end;
		return false;
	}

	return true;
}

// ── Playback completion ───────────────────────────────────────────────────────

void RuleEngine::markPlayed(const std::string& channel_id, const std::string& block_id,
							const std::string& item_type, const std::string& item_id,
							int64_t /*duration_ms*/)
{
	SQLite::Transaction txn(db_.get());

	ScheduleRepository(db_).recordPlayHistory(item_type, item_id, channel_id, block_id);

	// In 'scheduled' mode project() already advanced cursors during EPG generation,
	// so advancing again here would double-advance and skip episodes on the next
	// ensureScheduled() pass. Only advance on confirmed play for 'on_play' channels.
	if (!block_id.empty() && channelAdvanceMode(channel_id) == "on_play")
	{
		auto b = blocks_.loadBlock(block_id);
		if (b)
		{
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
