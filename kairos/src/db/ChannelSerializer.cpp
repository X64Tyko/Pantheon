#include "ChannelSerializer.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <chrono>
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

bool isValidTimezone(const std::string& tz) {
    try { std::chrono::locate_zone(tz); return true; } catch (...) { return false; }
}

} // namespace

ChannelSerializer::ChannelSerializer(Database& db) : db_(db), blocks_(db) {}

// ── Export ────────────────────────────────────────────────────────────────────

json ChannelSerializer::exportChannel(const std::string& channel_id, bool deep) {
    SQLite::Statement cq(db_.get(), R"(
        SELECT name, number, timezone, advance_mode, default_filler_selection, seed,
               offline_video_path, offline_image_path, offline_audio_type,
               offline_audio_title, logo_path
        FROM channel WHERE channel_id = ?
    )");
    cq.bind(1, channel_id);
    if (!cq.executeStep()) throw std::runtime_error("channel not found");

    json channel_j = {
        {"name",                     cq.getColumn(0).getString()},
        {"number",                   cq.getColumn(1).getInt()},
        {"timezone",                 cq.getColumn(2).getString()},
        {"advance_mode",             cq.getColumn(3).getString()},
        {"default_filler_selection", cq.getColumn(4).getString()},
        {"offline_video_path",       cq.getColumn(6).getString()},
        {"offline_image_path",       cq.getColumn(7).getString()},
        {"offline_audio_type",       cq.getColumn(8).getString()},
        {"offline_audio_title",      cq.getColumn(9).getString()},
        {"logo_path",                cq.getColumn(10).getString()},
    };
    if (!cq.getColumn(5).isNull()) channel_j["seed"] = cq.getColumn(5).getInt();

    // Channel filler entries
    SQLite::Statement cfq(db_.get(), R"(
        SELECT COALESCE(fl.title, pl.title, sh.title, mv.title, cfe.content_id),
               cfe.advancement, cfe.weight
        FROM channel_filler_entry cfe
        LEFT JOIN filler_list fl ON cfe.content_type='filler_list' AND fl.filler_list_id=cfe.content_id
        LEFT JOIN playlist    pl ON cfe.content_type='playlist'    AND pl.playlist_id=cfe.content_id
        LEFT JOIN show        sh ON cfe.content_type='show'        AND sh.show_id=cfe.content_id
        LEFT JOIN movie       mv ON cfe.content_type='movie'       AND mv.movie_id=cfe.content_id
        WHERE cfe.channel_id = ? ORDER BY cfe.position
    )");
    cfq.bind(1, channel_id);
    json ch_filler = json::array();
    while (cfq.executeStep()) {
        ch_filler.push_back({
            {"title",       cfq.getColumn(0).getString()},
            {"advancement", cfq.getColumn(1).getString()},
            {"weight",      cfq.getColumn(2).getInt()},
        });
    }
    channel_j["default_filler_entries"] = ch_filler;

    // Channel bumpers — slot resolution delegates to BlockSerializer
    SQLite::Statement bmpq(db_.get(), R"(
        SELECT cb.content_type, cb.mode, cb.every_n, cb.content_id,
               CASE cb.content_type
                   WHEN 'show'     THEN COALESCE(sw.title,'')
                   WHEN 'episode'  THEN COALESCE(esw.title,'') ||
                                        ' S' || PRINTF('%02d',e.season) ||
                                        'E' || PRINTF('%02d',e.episode)
                   WHEN 'playlist' THEN COALESCE(pl.title,'')
                   ELSE ''
               END AS title,
               COALESCE(sw.imdb_id,''), COALESCE(sw.tvdb_id,''), COALESCE(sw.tmdb_id,''),
               e.season, e.episode,
               COALESCE(esw.imdb_id,''), COALESCE(esw.tvdb_id,''), COALESCE(esw.tmdb_id,'')
        FROM channel_bumper cb
        LEFT JOIN show     sw  ON cb.content_type='show'     AND cb.content_id=sw.show_id
        LEFT JOIN episode  e   ON cb.content_type='episode'  AND cb.content_id=e.episode_id
        LEFT JOIN show     esw ON cb.content_type='episode'  AND e.show_id=esw.show_id
        LEFT JOIN playlist pl  ON cb.content_type='playlist' AND cb.content_id=pl.playlist_id
        WHERE cb.channel_id = ? ORDER BY cb.position
    )");
    bmpq.bind(1, channel_id);
    json bumpers = json::array();
    while (bmpq.executeStep()) {
        std::string bct = bmpq.getColumn(0).getString();
        json bmp = {
            {"content_type", bct},
            {"mode",         bmpq.getColumn(1).getString()},
            {"every_n",      bmpq.getColumn(2).getInt()},
            {"title",        bmpq.getColumn(4).getString()},
        };
        if (deep) {
            if (bct == "show") {
                bmp["imdb_id"] = bmpq.getColumn(5).getString();
                bmp["tvdb_id"] = bmpq.getColumn(6).getString();
                bmp["tmdb_id"] = bmpq.getColumn(7).getString();
            } else if (bct == "episode") {
                if (!bmpq.getColumn(8).isNull())  bmp["season"]       = bmpq.getColumn(8).getInt();
                if (!bmpq.getColumn(9).isNull())  bmp["episode"]      = bmpq.getColumn(9).getInt();
                bmp["show_imdb_id"] = bmpq.getColumn(10).getString();
                bmp["show_tvdb_id"] = bmpq.getColumn(11).getString();
                bmp["show_tmdb_id"] = bmpq.getColumn(12).getString();
            }
        }
        bumpers.push_back(bmp);
    }
    channel_j["bumpers"] = bumpers;

    // Blocks — each block serializes itself
    SQLite::Statement bq(db_.get(),
        "SELECT block_id FROM block WHERE channel_id=? ORDER BY start_time, priority DESC");
    bq.bind(1, channel_id);
    json blocks = json::array();
    while (bq.executeStep())
        blocks.push_back(blocks_.exportBlock(bq.getColumn(0).getString(), deep));

    return json{
        {"kairos_export", 1},
        {"depth",         deep ? "deep" : "shallow"},
        {"channel",       channel_j},
        {"blocks",        blocks},
    };
}

// ── Preview ───────────────────────────────────────────────────────────────────

json ChannelSerializer::previewImport(const json& body, bool deep) {
    int  unresolved_count = 0;
    json preview_blocks   = json::array();

    for (const auto& blk : body.value("blocks", json::array())) {
        json block_out = {
            {"name",        blk.value("name",        "")},
            {"block_type",  blk.value("block_type",  "episode")},
            {"advancement", blk.value("advancement", "sequential")},
            {"day_mask",    blk.value("day_mask",    127)},
            {"start_time",  blk.value("start_time",  "00:00")},
            {"content",     json::array()},
        };
        if (blk.value("end_time",     "").size()) block_out["end_time"]      = blk["end_time"];
        if (blk.value("program_count", 0) > 0)   block_out["program_count"] = blk["program_count"];

        for (const auto& item : blk.value("content", json::array())) {
            std::string ct    = item.value("content_type", "");
            bool resolved     = !blocks_.resolveSlot(item, deep).empty();
            if (!resolved) ++unresolved_count;

            json out = {{"content_type", ct}, {"title", item.value("title","")}, {"resolved", resolved}};
            for (const char* k : {"tvdb_id","imdb_id","tmdb_id","year","season_filter"})
                if (item.contains(k) && !item[k].is_null()) out[k] = item[k];
            block_out["content"].push_back(out);
        }
        preview_blocks.push_back(block_out);
    }
    return json{{"blocks", preview_blocks}, {"unresolved_count", unresolved_count}};
}

// ── Import ────────────────────────────────────────────────────────────────────

ChannelSerializer::ImportResult ChannelSerializer::importChannel(const json& body, bool deep) {
    const json& ch = body.at("channel");

    std::string channel_id = generateId();

    SQLite::Transaction txn(db_.get());

    // Insert channel row
    {
        std::string tz = ch.value("timezone", "UTC");
        if (!isValidTimezone(tz)) tz = "UTC";
        SQLite::Statement s(db_.get(), R"(
            INSERT INTO channel (channel_id, name, number, timezone, advance_mode,
                                 default_filler_selection, offline_video_path,
                                 offline_image_path, logo_path)
            VALUES (?,?,?,?,?,?,?,?,?)
        )");
        s.bind(1, channel_id);
        s.bind(2, ch.value("name",                     "Imported Channel"));
        s.bind(3, ch.value("number",                   1));
        s.bind(4, tz);
        s.bind(5, ch.value("advance_mode",             "scheduled"));
        s.bind(6, ch.value("default_filler_selection", "round_robin"));
        s.bind(7, ch.value("offline_video_path",       ""));
        s.bind(8, ch.value("offline_image_path",       ""));
        s.bind(9, ch.value("logo_path",                ""));
        s.exec();
    }
    if (ch.contains("seed") && !ch["seed"].is_null()) {
        SQLite::Statement s(db_.get(), "UPDATE channel SET seed=? WHERE channel_id=?");
        s.bind(1, ch["seed"].get<int>()); s.bind(2, channel_id); s.exec();
    }

    // Offline audio — resolve by type + title
    {
        std::string oa_type  = ch.value("offline_audio_type",  "");
        std::string oa_title = ch.value("offline_audio_title", "");
        if (!oa_type.empty() && !oa_title.empty()) {
            const char* sql = (oa_type == "movie")
                ? "SELECT movie_id   FROM movie   WHERE title=? LIMIT 1"
                : "SELECT episode_id FROM episode WHERE title=? LIMIT 1";
            SQLite::Statement q(db_.get(), sql);
            q.bind(1, oa_title);
            if (q.executeStep()) {
                std::string oa_id = q.getColumn(0).getString();
                SQLite::Statement u(db_.get(), R"(
                    UPDATE channel SET offline_audio_id=?, offline_audio_type=?, offline_audio_title=?
                    WHERE channel_id=?
                )");
                u.bind(1, oa_id); u.bind(2, oa_type); u.bind(3, oa_title);
                u.bind(4, channel_id); u.exec();
            }
        }
    }

    // Channel filler entries
    int cfpos = 0;
    for (const auto& fe : ch.value("default_filler_entries", json::array())) {
        SQLite::Statement fq(db_.get(),
            "SELECT filler_list_id FROM filler_list WHERE title=? LIMIT 1");
        fq.bind(1, fe.value("title", ""));
        if (!fq.executeStep()) { cfpos++; continue; }
        std::string fid = fq.getColumn(0).getString();
        SQLite::Statement ins(db_.get(), R"(
            INSERT OR IGNORE INTO channel_filler_entry
                (channel_id, content_type, content_id, advancement, weight, position)
            VALUES (?,?,?,?,?,?)
        )");
        ins.bind(1, channel_id); ins.bind(2, std::string("filler_list")); ins.bind(3, fid);
        ins.bind(4, fe.value("advancement", "sized")); ins.bind(5, fe.value("weight", 1));
        ins.bind(6, cfpos++); ins.exec();
    }

    // Channel bumpers — resolveSlot delegated to blocks_
    {
        int bmppos = 0;
        for (const auto& bmp : ch.value("bumpers", json::array())) {
            std::string bct    = bmp.value("content_type", "");
            std::string bmp_cid = blocks_.resolveSlot(bmp, deep);
            if (bmp_cid.empty()) { bmppos++; continue; }
            SQLite::Statement ins(db_.get(), R"(
                INSERT INTO channel_bumper
                    (channel_id, content_type, content_id, mode, every_n, position)
                VALUES (?,?,?,?,?,?)
            )");
            ins.bind(1, channel_id); ins.bind(2, bct); ins.bind(3, bmp_cid);
            ins.bind(4, bmp.value("mode",    "between"));
            ins.bind(5, bmp.value("every_n", 3));
            ins.bind(6, bmppos++); ins.exec();
        }
    }

    // Blocks — each block imports itself
    json unresolved = json::array();
    for (const auto& blk : body.value("blocks", json::array()))
        blocks_.importBlock(channel_id, blk, deep, unresolved);

    txn.commit();
    return {channel_id, unresolved};
}
