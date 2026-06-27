#include "BlockSerializer.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <iomanip>
#include <random>
#include <sstream>

using json = nlohmann::json;

namespace {

std::string generateId() {
    thread_local std::mt19937_64 rng(std::random_device{}());
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << rng();
    return ss.str();
}

} // namespace

BlockSerializer::BlockSerializer(Database& db) : db_(db) {}

// ── Slot helpers ──────────────────────────────────────────────────────────────

std::string BlockSerializer::resolveSlot(const json& slot, bool deep) {
    if (!slot.is_object()) return "";
    std::string ct    = slot.value("content_type", "");
    std::string title = slot.value("title", "");
    std::string cid;

    auto tryQ = [&](const char* sql, const std::string& val) {
        if (!cid.empty() || val.empty()) return;
        SQLite::Statement q(db_.get(), sql);
        q.bind(1, val);
        if (q.executeStep()) cid = q.getColumn(0).getString();
    };

    if (ct == "show") {
        if (deep) {
            tryQ("SELECT show_id FROM show WHERE imdb_id=? AND imdb_id!='' LIMIT 1", slot.value("imdb_id",""));
            tryQ("SELECT show_id FROM show WHERE tvdb_id=? AND tvdb_id!='' LIMIT 1", slot.value("tvdb_id",""));
            tryQ("SELECT show_id FROM show WHERE tmdb_id=? AND tmdb_id!='' LIMIT 1", slot.value("tmdb_id",""));
        }
        if (cid.empty() && slot.contains("year") && !slot["year"].is_null()) {
            SQLite::Statement q(db_.get(), "SELECT show_id FROM show WHERE title=? AND year=? LIMIT 1");
            q.bind(1, title); q.bind(2, slot["year"].get<int>());
            if (q.executeStep()) cid = q.getColumn(0).getString();
        }
        tryQ("SELECT show_id FROM show WHERE title=? LIMIT 1", title);
    } else if (ct == "movie") {
        if (deep) {
            tryQ("SELECT movie_id FROM movie WHERE imdb_id=? AND imdb_id!='' LIMIT 1", slot.value("imdb_id",""));
            tryQ("SELECT movie_id FROM movie WHERE tmdb_id=? AND tmdb_id!='' LIMIT 1", slot.value("tmdb_id",""));
        }
        if (cid.empty() && slot.contains("year") && !slot["year"].is_null()) {
            SQLite::Statement q(db_.get(), "SELECT movie_id FROM movie WHERE title=? AND year=? LIMIT 1");
            q.bind(1, title); q.bind(2, slot["year"].get<int>());
            if (q.executeStep()) cid = q.getColumn(0).getString();
        }
        tryQ("SELECT movie_id FROM movie WHERE title=? LIMIT 1", title);
    } else if (ct == "episode" && deep) {
        std::string show_id;
        auto tryShow = [&](const char* col, const std::string& val) {
            if (!show_id.empty() || val.empty()) return;
            SQLite::Statement q(db_.get(),
                std::string("SELECT show_id FROM show WHERE ") + col + "=? AND " + col + "!='' LIMIT 1");
            q.bind(1, val);
            if (q.executeStep()) show_id = q.getColumn(0).getString();
        };
        tryShow("imdb_id", slot.value("show_imdb_id",""));
        tryShow("tvdb_id", slot.value("show_tvdb_id",""));
        tryShow("tmdb_id", slot.value("show_tmdb_id",""));
        if (!show_id.empty()) {
            SQLite::Statement q(db_.get(),
                "SELECT episode_id FROM episode WHERE show_id=? AND season=? AND episode=? LIMIT 1");
            q.bind(1, show_id);
            q.bind(2, slot.value("season",  0));
            q.bind(3, slot.value("episode", 0));
            if (q.executeStep()) cid = q.getColumn(0).getString();
        }
    } else if (ct == "playlist") {
        tryQ("SELECT playlist_id FROM playlist WHERE title=? LIMIT 1", title);
    } else if (ct == "filler_list") {
        tryQ("SELECT filler_list_id FROM filler_list WHERE title=? LIMIT 1", title);
    }
    return cid;
}

json BlockSerializer::exportSlot(const std::string& ct, const std::string& cid, bool deep) {
    if (ct.empty() || cid.empty()) return nullptr;
    json slot = {{"content_type", ct}};

    if (ct == "show") {
        SQLite::Statement q(db_.get(),
            "SELECT title, imdb_id, tvdb_id, tmdb_id, year FROM show WHERE show_id=? LIMIT 1");
        q.bind(1, cid);
        if (!q.executeStep()) return nullptr;
        slot["title"] = q.getColumn(0).getString();
        if (!q.getColumn(4).isNull()) slot["year"] = q.getColumn(4).getInt();
        if (deep) {
            slot["imdb_id"] = q.getColumn(1).getString();
            slot["tvdb_id"] = q.getColumn(2).getString();
            slot["tmdb_id"] = q.getColumn(3).getString();
        }
    } else if (ct == "movie") {
        SQLite::Statement q(db_.get(),
            "SELECT title, imdb_id, tmdb_id, year FROM movie WHERE movie_id=? LIMIT 1");
        q.bind(1, cid);
        if (!q.executeStep()) return nullptr;
        slot["title"] = q.getColumn(0).getString();
        if (!q.getColumn(3).isNull()) slot["year"] = q.getColumn(3).getInt();
        if (deep) {
            slot["imdb_id"] = q.getColumn(1).getString();
            slot["tmdb_id"] = q.getColumn(2).getString();
        }
    } else if (ct == "episode") {
        SQLite::Statement q(db_.get(), R"(
            SELECT esw.title, e.season, e.episode, esw.imdb_id, esw.tvdb_id, esw.tmdb_id
            FROM episode e JOIN show esw ON e.show_id = esw.show_id
            WHERE e.episode_id=? LIMIT 1
        )");
        q.bind(1, cid);
        if (!q.executeStep()) return nullptr;
        std::ostringstream oss;
        oss << q.getColumn(0).getString()
            << " S" << std::setw(2) << std::setfill('0') << q.getColumn(1).getInt()
            << "E"  << std::setw(2) << std::setfill('0') << q.getColumn(2).getInt();
        slot["title"] = oss.str();
        if (deep) {
            slot["season"]       = q.getColumn(1).getInt();
            slot["episode"]      = q.getColumn(2).getInt();
            slot["show_imdb_id"] = q.getColumn(3).getString();
            slot["show_tvdb_id"] = q.getColumn(4).getString();
            slot["show_tmdb_id"] = q.getColumn(5).getString();
        }
    } else if (ct == "playlist") {
        SQLite::Statement q(db_.get(), "SELECT title FROM playlist WHERE playlist_id=? LIMIT 1");
        q.bind(1, cid);
        if (!q.executeStep()) return nullptr;
        slot["title"] = q.getColumn(0).getString();
    } else if (ct == "filler_list") {
        SQLite::Statement q(db_.get(), "SELECT title FROM filler_list WHERE filler_list_id=? LIMIT 1");
        q.bind(1, cid);
        if (!q.executeStep()) return nullptr;
        slot["title"] = q.getColumn(0).getString();
    }
    return slot;
}

// ── Export ────────────────────────────────────────────────────────────────────

json BlockSerializer::exportBlock(const std::string& block_id, bool deep) {
    SQLite::Statement bq(db_.get(), R"(
        SELECT name, block_type, day_mask, start_time, end_time,
               program_count, priority, play_style, advancement, cursor_scope,
               late_start_mins, align_to_mins, inter_filler, early_start_secs,
               filler_selection, smart_pct, start_scope, no_history_behavior,
               max_consecutive_episodes,
               intro_content_type, intro_content_id,
               outro_content_type, outro_content_id,
               interstitial_content_type, interstitial_content_id,
               interstitial_every_n, snap_to_group_start
        FROM block WHERE block_id = ?
    )");
    bq.bind(1, block_id);
    if (!bq.executeStep()) throw std::runtime_error("block not found: " + block_id);

    json block_j = {
        {"name",                     bq.getColumn(0).getString()},
        {"block_type",               bq.getColumn(1).getString()},
        {"day_mask",                 bq.getColumn(2).getInt()},
        {"start_time",               bq.getColumn(3).getString()},
        {"program_count",            bq.getColumn(5).getInt()},
        {"priority",                 bq.getColumn(6).getInt()},
        {"play_style",               bq.getColumn(7).getString()},
        {"advancement",              bq.getColumn(8).getString()},
        {"cursor_scope",             bq.getColumn(9).getString()},
        {"late_start_mins",          bq.getColumn(10).getInt()},
        {"align_to_mins",            bq.getColumn(11).getInt()},
        {"inter_filler",             bq.getColumn(12).getInt() != 0},
        {"early_start_secs",         bq.getColumn(13).getInt()},
        {"filler_selection",         bq.getColumn(14).getString()},
        {"smart_pct",                bq.getColumn(15).getInt()},
        {"start_scope",              bq.getColumn(16).getString()},
        {"no_history_behavior",      bq.getColumn(17).getString()},
        {"max_consecutive_episodes", bq.getColumn(18).getInt()},
        {"interstitial_every_n",     bq.getColumn(25).getInt()},
        {"snap_to_group_start",      bq.getColumn(26).getInt() != 0},
    };
    if (!bq.getColumn(4).isNull()) block_j["end_time"] = bq.getColumn(4).getString();

    json intro = exportSlot(bq.getColumn(19).getString(), bq.getColumn(20).getString(), deep);
    json outro = exportSlot(bq.getColumn(21).getString(), bq.getColumn(22).getString(), deep);
    json inter = exportSlot(bq.getColumn(23).getString(), bq.getColumn(24).getString(), deep);
    if (!intro.is_null()) block_j["intro"]        = std::move(intro);
    if (!outro.is_null()) block_j["outro"]        = std::move(outro);
    if (!inter.is_null()) block_j["interstitial"] = std::move(inter);

    // Content items
    SQLite::Statement ccq(db_.get(), R"(
        SELECT bc.content_type, bc.weight, bc.run_count, bc.season_filter,
               CASE bc.content_type
                   WHEN 'show'        THEN COALESCE(sw.title,'')
                   WHEN 'movie'       THEN COALESCE(m.title,'')
                   WHEN 'episode'     THEN COALESCE(esw.title,'') ||
                                           ' S' || PRINTF('%02d',e.season) ||
                                           'E' || PRINTF('%02d',e.episode)
                   WHEN 'playlist'    THEN COALESCE(pl.title,'')
                   WHEN 'filler_list' THEN COALESCE(fl.title,'')
                   ELSE ''
               END AS title,
               COALESCE(sw.imdb_id,''), COALESCE(sw.tvdb_id,''), COALESCE(sw.tmdb_id,''),
               COALESCE(m.imdb_id,''),  COALESCE(m.tmdb_id,''),
               e.season, e.episode,
               COALESCE(esw.imdb_id,''), COALESCE(esw.tvdb_id,''), COALESCE(esw.tmdb_id,''),
               sw.year, m.year
        FROM block_content bc
        LEFT JOIN show        sw  ON bc.content_type='show'        AND bc.content_id=sw.show_id
        LEFT JOIN movie       m   ON bc.content_type='movie'       AND bc.content_id=m.movie_id
        LEFT JOIN episode     e   ON bc.content_type='episode'     AND bc.content_id=e.episode_id
        LEFT JOIN show        esw ON bc.content_type='episode'     AND e.show_id=esw.show_id
        LEFT JOIN playlist    pl  ON bc.content_type='playlist'    AND bc.content_id=pl.playlist_id
        LEFT JOIN filler_list fl  ON bc.content_type='filler_list' AND bc.content_id=fl.filler_list_id
        WHERE bc.block_id = ? ORDER BY bc.position
    )");
    ccq.bind(1, block_id);
    json content = json::array();
    while (ccq.executeStep()) {
        std::string ct = ccq.getColumn(0).getString();
        json item = {
            {"content_type", ct},
            {"weight",       ccq.getColumn(1).getInt()},
            {"run_count",    ccq.getColumn(2).getInt()},
            {"title",        ccq.getColumn(4).getString()},
        };
        if (!ccq.getColumn(3).isNull()) item["season_filter"] = ccq.getColumn(3).getInt();
        if (ct == "show"  && !ccq.getColumn(15).isNull()) item["year"] = ccq.getColumn(15).getInt();
        if (ct == "movie" && !ccq.getColumn(16).isNull()) item["year"] = ccq.getColumn(16).getInt();
        if (deep) {
            if (ct == "show") {
                item["imdb_id"] = ccq.getColumn(5).getString();
                item["tvdb_id"] = ccq.getColumn(6).getString();
                item["tmdb_id"] = ccq.getColumn(7).getString();
            } else if (ct == "movie") {
                item["imdb_id"] = ccq.getColumn(8).getString();
                item["tmdb_id"] = ccq.getColumn(9).getString();
            } else if (ct == "episode") {
                if (!ccq.getColumn(10).isNull()) item["season"]  = ccq.getColumn(10).getInt();
                if (!ccq.getColumn(11).isNull()) item["episode"] = ccq.getColumn(11).getInt();
                item["show_imdb_id"] = ccq.getColumn(12).getString();
                item["show_tvdb_id"] = ccq.getColumn(13).getString();
                item["show_tmdb_id"] = ccq.getColumn(14).getString();
            }
        }
        content.push_back(item);
    }
    block_j["content"] = content;

    // Filler entries
    SQLite::Statement bfq(db_.get(), R"(
        SELECT bfe.id, bfe.content_type, bfe.content_id,
               COALESCE(fl.title, pl.title, sh.title, mv.title, bfe.content_id),
               bfe.advancement, bfe.weight, bfe.position, bfe.season_filter
        FROM block_filler_entry bfe
        LEFT JOIN filler_list fl ON bfe.content_type='filler_list' AND fl.filler_list_id=bfe.content_id
        LEFT JOIN playlist    pl ON bfe.content_type='playlist'    AND pl.playlist_id=bfe.content_id
        LEFT JOIN show        sh ON bfe.content_type='show'        AND sh.show_id=bfe.content_id
        LEFT JOIN movie       mv ON bfe.content_type='movie'       AND mv.movie_id=bfe.content_id
        WHERE bfe.block_id = ? ORDER BY bfe.position
    )");
    bfq.bind(1, block_id);
    json filler = json::array();
    while (bfq.executeStep()) {
        json bfe = {
            {"id",           bfq.getColumn(0).getInt()},
            {"content_type", bfq.getColumn(1).getString()},
            {"content_id",   bfq.getColumn(2).getString()},
            {"title",        bfq.getColumn(3).getString()},
            {"advancement",  bfq.getColumn(4).getString()},
            {"weight",       bfq.getColumn(5).getInt()},
            {"position",     bfq.getColumn(6).getInt()},
        };
        if (!bfq.getColumn(7).isNull()) bfe["season_filter"] = bfq.getColumn(7).getInt();
        filler.push_back(bfe);
    }
    block_j["filler_entries"] = filler;

    return block_j;
}

// ── Import ────────────────────────────────────────────────────────────────────

std::string BlockSerializer::importBlock(const std::string& channel_id,
                                          const json& blk,
                                          bool deep,
                                          json& unresolved) {
    std::string block_id   = generateId();
    std::string end_time   = blk.value("end_time", "");
    std::string block_name = blk.value("name", "");

    SQLite::Statement s(db_.get(), R"(
        INSERT INTO block (block_id, channel_id, name, block_type, day_mask,
                           start_time, end_time, program_count, priority,
                           max_content_rating, advancement, cursor_scope,
                           late_start_mins, align_to_mins, inter_filler,
                           early_start_secs, filler_selection, smart_pct,
                           start_scope, no_history_behavior,
                           max_consecutive_episodes, interstitial_every_n,
                           snap_to_group_start)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    )");
    s.bind(1,  block_id); s.bind(2, channel_id);
    s.bind(3,  block_name);
    s.bind(4,  blk.value("block_type",           "episode"));
    s.bind(5,  blk.value("day_mask",             127));
    s.bind(6,  blk.value("start_time",           "00:00"));
    if (end_time.empty()) s.bind(7); else s.bind(7, end_time);
    s.bind(8,  blk.value("program_count",        0));
    s.bind(9,  blk.value("priority",             0));
    s.bind(10, blk.value("play_style",            "standard"));
    s.bind(11, blk.value("advancement",          "sequential"));
    s.bind(12, blk.value("cursor_scope",         "block"));
    s.bind(13, blk.value("late_start_mins",      0));
    s.bind(14, blk.value("align_to_mins",        0));
    s.bind(15, blk.value("inter_filler", false) ? 1 : 0);
    s.bind(16, blk.value("early_start_secs",           0));
    s.bind(17, blk.value("filler_selection",     "round_robin"));
    s.bind(18, blk.value("smart_pct",            30));
    s.bind(19, blk.value("start_scope",          "block"));
    s.bind(20, blk.value("no_history_behavior",  "normal"));
    s.bind(21, blk.value("max_consecutive_episodes",   0));
    s.bind(22, blk.value("interstitial_every_n",       1));
    s.bind(23, blk.value("snap_to_group_start", true) ? 1 : 0);
    s.exec();

    // Content items
    int pos = 0;
    for (const auto& item : blk.value("content", json::array())) {
        std::string ct         = item.value("content_type", "");
        std::string content_id = resolveSlot(item, deep);

        if (content_id.empty()) {
            unresolved.push_back({
                {"block_name",   block_name},
                {"content_type", ct},
                {"title",        item.value("title", "")},
                {"reason",       "no match found"},
            });
            pos++; continue;
        }

        SQLite::Statement ins(db_.get(), R"(
            INSERT OR IGNORE INTO block_content
                (block_id, content_type, content_id, position, weight, run_count)
            VALUES (?,?,?,?,?,?)
        )");
        ins.bind(1, block_id); ins.bind(2, ct); ins.bind(3, content_id);
        ins.bind(4, pos);
        ins.bind(5, item.value("weight",    1));
        ins.bind(6, item.value("run_count", 1));
        ins.exec();

        if (item.contains("season_filter") && !item["season_filter"].is_null()) {
            SQLite::Statement upd(db_.get(), R"(
                UPDATE block_content SET season_filter=?
                WHERE block_id=? AND content_type=? AND content_id=?
            )");
            upd.bind(1, item["season_filter"].get<int>());
            upd.bind(2, block_id); upd.bind(3, ct); upd.bind(4, content_id);
            upd.exec();
        }
        pos++;
    }

    // Filler entries (import always resolves to filler_list for compat)
    for (const auto& fe : blk.value("filler_entries", json::array())) {
        SQLite::Statement fq(db_.get(),
            "SELECT filler_list_id FROM filler_list WHERE title=? LIMIT 1");
        fq.bind(1, fe.value("title", ""));
        if (!fq.executeStep()) continue;
        std::string fid = fq.getColumn(0).getString();
        SQLite::Statement pq(db_.get(),
            "SELECT COALESCE(MAX(position),-1)+1 FROM block_filler_entry WHERE block_id=?");
        pq.bind(1, block_id); pq.executeStep();
        SQLite::Statement ins(db_.get(), R"(
            INSERT OR IGNORE INTO block_filler_entry
                (block_id, content_type, content_id, advancement, weight, position)
            VALUES (?,?,?,?,?,?)
        )");
        ins.bind(1, block_id); ins.bind(2, std::string("filler_list")); ins.bind(3, fid);
        ins.bind(4, fe.value("advancement", "sized")); ins.bind(5, fe.value("weight", 1));
        ins.bind(6, pq.getColumn(0).getInt()); ins.exec();
    }

    // Intro / outro / interstitial slots
    auto applySlot = [&](const char* type_col, const char* id_col, const json& slot) {
        if (!slot.is_object()) return;
        std::string ct  = slot.value("content_type", "");
        std::string cid = resolveSlot(slot, deep);
        if (cid.empty()) return;
        SQLite::Statement u(db_.get(),
            std::string("UPDATE block SET ") + type_col + "=?, " + id_col + "=? WHERE block_id=?");
        u.bind(1, ct); u.bind(2, cid); u.bind(3, block_id); u.exec();
    };
    if (blk.contains("intro"))        applySlot("intro_content_type",        "intro_content_id",        blk["intro"]);
    if (blk.contains("outro"))        applySlot("outro_content_type",        "outro_content_id",        blk["outro"]);
    if (blk.contains("interstitial")) applySlot("interstitial_content_type", "interstitial_content_id", blk["interstitial"]);

    return block_id;
}
