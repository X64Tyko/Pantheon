#include "RuleEngine.h"
#include "../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ctime>
#include <iostream>

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
    if (s == "rerun_shuffle") return Advancement::RerunShuffle;
    return Advancement::Sequential;
}

static CursorScope parseCursorScope(const std::string& s) {
    if (s == "global")  return CursorScope::Global;
    if (s == "channel") return CursorScope::Channel;
    return CursorScope::Block;
}

// ── SimState JSON serialization ───────────────────────────────────────────────

static json simStateToJson(const RuleEngine::SimState& s) {
    json j;
    j["show_pos"] = json::object();
    j["block_rr"] = json::object();
    for (const auto& [k, v] : s.show_pos) j["show_pos"][k] = v;
    for (const auto& [k, v] : s.block_rr)  j["block_rr"][k]  = v;
    return j;
}

static RuleEngine::SimState simStateFromJson(const std::string& raw) {
    RuleEngine::SimState s;
    if (raw.empty() || raw == "{}") return s;
    try {
        auto j = json::parse(raw);
        if (j.contains("show_pos"))
            for (auto& [k, v] : j["show_pos"].items()) s.show_pos[k] = v.get<int>();
        if (j.contains("block_rr"))
            for (auto& [k, v] : j["block_rr"].items())  s.block_rr[k]  = v.get<int>();
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
               late_start_mins, align_to_mins, inter_filler, early_start_secs, filler_selection
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

        SQLite::Statement cq(db_.get(), R"(
            SELECT id, content_type, content_id, position, season_filter
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
            b.content.push_back(std::move(bc));
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
                                                  std::time_t t) {
    auto tm      = toUTC(t);
    int  day_bit = dayBit(tm);
    int  cur_min = tm.tm_hour * 60 + tm.tm_min;

    const Block* best = nullptr;
    for (const auto& b : blocks) {
        if (!(b.day_mask & day_bit)) continue;
        int start_min = parseTimeMins(b.start_time);
        if (cur_min < start_min) continue;
        if (b.end_time.has_value()) {
            if (cur_min >= parseTimeMins(*b.end_time)) continue;
        }
        // blocks are pre-sorted priority DESC; first match wins
        if (!best) best = &b;
    }
    if (!best) return std::nullopt;
    return *best;
}

std::optional<Block> RuleEngine::resolveBlock(const std::string& channel_id, std::time_t t) {
    auto blocks = loadBlocks(channel_id);
    return resolveFromList(blocks, t);
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
        auto eps = getEpisodes(bc.content_id, bc.season_filter);
        if (eps.empty()) return std::nullopt;
        int pos = readCursorPos("show", bc.content_id, scopeStr(block), scopeId(block, channel_id));
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
        auto eps = getEpisodes(bc.content_id, bc.season_filter);
        if (eps.empty()) { rr = (rr + 1) % n; return std::nullopt; }

        std::string key = scopeStr(block) + ":" + scopeId(block, channel_id) + ":" + bc.content_id;
        if (!state.show_pos.count(key))
            state.show_pos[key] = (seed >= 0)
                ? (seed % static_cast<int>(eps.size()))
                : readCursorPos("show", bc.content_id, scopeStr(block), scopeId(block, channel_id));
        auto& ep_pos = state.show_pos[key];
        auto item = itemFromShow(channel_id, block.block_id, eps, ep_pos, showTitle(bc.content_id));
        ep_pos = (ep_pos + 1) % static_cast<int>(eps.size());
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
        if (!state.block_rr.count(b.block_id)) {
            if (seed >= 0) {
                int n = static_cast<int>(b.content.size());
                state.block_rr[b.block_id] = n > 0 ? seed % n : 0;
            } else {
                state.block_rr[b.block_id] = readBlockRR(b.block_id, channel_id);
            }
        }
    }

    std::time_t t   = start;
    std::time_t end = start + static_cast<std::time_t>(horizon_hours) * 3600;
    const int MAX_ITEMS = 500;

    while (t < end && static_cast<int>(result.size()) < MAX_ITEMS) {
        auto block_opt = resolveFromList(blocks, t);
        if (!block_opt) {
            t += 1800; // no block active — skip 30 min
            continue;
        }
        const Block& block = *block_opt;

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
            auto  tm_t    = toUTC(t);
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
                auto eps = getEpisodes(bc.content_id, bc.season_filter);
                if (!eps.empty()) {
                    std::string scope    = scopeStr(b);
                    std::string scope_id = scopeId(b, channel_id);
                    int pos = readCursorPos("show", bc.content_id, scope, scope_id);
                    int next_pos = (pos + 1) % static_cast<int>(eps.size());
                    writeCursorPos("show", bc.content_id, scope, scope_id,
                                   next_pos, eps[next_pos].episode_id);
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
