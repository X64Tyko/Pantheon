#include "PlaylistRepository.h"
#include "Database.h"
#include "DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>

PlaylistRepository::PlaylistRepository(Database& db) : db_(db) {}

std::string PlaylistRepository::create(const std::string& title, const std::string& mode) {
    std::string playlist_id = db::generateId();
    SQLite::Statement s(db_.get(),
        "INSERT INTO playlist (playlist_id, title, source, mode) VALUES (?,?,'custom',?)");
    s.bind(1, playlist_id); s.bind(2, title); s.bind(3, mode);
    s.exec();
    return playlist_id;
}

void PlaylistRepository::updateField(const std::string& playlist_id,
                                      const std::string& col,
                                      const std::string& val) {
    SQLite::Statement s(db_.get(),
        "UPDATE playlist SET " + col + " = ? WHERE playlist_id = ?");
    s.bind(1, val); s.bind(2, playlist_id); s.exec();
}

void PlaylistRepository::remove(const std::string& playlist_id) {
    SQLite::Statement s(db_.get(), "DELETE FROM playlist WHERE playlist_id = ?");
    s.bind(1, playlist_id); s.exec();
}

void PlaylistRepository::unlinkPlex(const std::string& playlist_id) {
    SQLite::Statement s(db_.get(),
        "DELETE FROM plex_list_link WHERE list_type = 'playlist' AND list_id = ?");
    s.bind(1, playlist_id); s.exec();
}

std::pair<int64_t, int> PlaylistRepository::addItem(const std::string& playlist_id,
                                                      const std::string& item_type,
                                                      const std::string& item_id) {
    int position = 0;
    {
        SQLite::Statement pq(db_.get(),
            "SELECT COALESCE(MAX(position), -1) + 1 FROM playlist_item WHERE playlist_id = ?");
        pq.bind(1, playlist_id);
        if (pq.executeStep()) position = pq.getColumn(0).getInt();
    }
    SQLite::Statement s(db_.get(),
        "INSERT INTO playlist_item (playlist_id, position, item_type, item_id) VALUES (?,?,?,?)");
    s.bind(1, playlist_id); s.bind(2, position); s.bind(3, item_type); s.bind(4, item_id);
    s.exec();
    return {db_.get().getLastInsertRowid(), position};
}

int PlaylistRepository::addItems(const std::string& playlist_id,
                                  const std::vector<std::pair<std::string, std::string>>& items) {
    SQLite::Statement pq(db_.get(),
        "SELECT COALESCE(MAX(position), -1) + 1 FROM playlist_item WHERE playlist_id = ?");
    pq.bind(1, playlist_id);
    int position = pq.executeStep() ? pq.getColumn(0).getInt() : 0;
    int added = 0;
    SQLite::Transaction tx(db_.get());
    for (const auto& [item_type, item_id] : items) {
        if (item_id.empty()) continue;
        try {
            SQLite::Statement s(db_.get(),
                "INSERT INTO playlist_item (playlist_id, position, item_type, item_id) VALUES (?,?,?,?)");
            s.bind(1, playlist_id); s.bind(2, position); s.bind(3, item_type); s.bind(4, item_id);
            s.exec();
            ++position; ++added;
        } catch (const SQLite::Exception&) { /* skip duplicates */ }
    }
    tx.commit();
    return added;
}

void PlaylistRepository::removeItem(int item_id, const std::string& playlist_id) {
    int pos = -1;
    {
        SQLite::Statement q(db_.get(),
            "SELECT position FROM playlist_item WHERE id = ? AND playlist_id = ?");
        q.bind(1, item_id); q.bind(2, playlist_id);
        if (!q.executeStep()) return;
        pos = q.getColumn(0).getInt();
    }
    SQLite::Statement del(db_.get(), "DELETE FROM playlist_item WHERE id = ?");
    del.bind(1, item_id); del.exec();
    SQLite::Statement ren(db_.get(),
        "UPDATE playlist_item SET position = position - 1 WHERE playlist_id = ? AND position > ?");
    ren.bind(1, playlist_id); ren.bind(2, pos); ren.exec();
}

void PlaylistRepository::moveItem(int item_id, const std::string& playlist_id, int new_position) {
    SQLite::Statement s(db_.get(),
        "UPDATE playlist_item SET position = ? WHERE id = ? AND playlist_id = ?");
    s.bind(1, new_position); s.bind(2, item_id); s.bind(3, playlist_id); s.exec();
}

std::vector<PlaylistRow> PlaylistRepository::listAll() {
    SQLite::Statement q(db_.get(), R"(
        SELECT p.playlist_id, p.title, p.mode,
               COUNT(pi.id) AS item_count,
               COALESCE(SUM(CASE pi.item_type
                   WHEN 'episode' THEN e.duration_ms
                   WHEN 'movie'   THEN m.duration_ms ELSE 0 END), 0) AS total_ms,
               pll.source_id, pll.external_id, pll.plex_type, pll.last_synced_at
        FROM playlist p
        LEFT JOIN playlist_item pi ON pi.playlist_id = p.playlist_id
        LEFT JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
        LEFT JOIN movie   m ON pi.item_type = 'movie'   AND pi.item_id = m.movie_id
        LEFT JOIN plex_list_link pll ON pll.list_type = 'playlist' AND pll.list_id = p.playlist_id
        GROUP BY p.playlist_id ORDER BY p.title
    )");
    std::vector<PlaylistRow> rows;
    while (q.executeStep()) {
        PlaylistRow r;
        r.playlist_id = q.getColumn(0).getString();
        r.title       = q.getColumn(1).getString();
        r.mode        = q.getColumn(2).getString();
        r.item_count  = q.getColumn(3).getInt();
        r.total_ms    = q.getColumn(4).getInt64();
        if (!q.getColumn(5).isNull()) {
            PlexLinkRow pl;
            pl.source_id   = q.getColumn(5).getString();
            pl.external_id = q.getColumn(6).getString();
            pl.plex_type   = q.getColumn(7).getString();
            if (!q.getColumn(8).isNull()) pl.last_synced_at = q.getColumn(8).getInt64();
            r.plex_link = std::move(pl);
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

std::optional<PlaylistDetail> PlaylistRepository::getDetail(const std::string& playlist_id) {
    SQLite::Statement ph(db_.get(),
        "SELECT playlist_id, title, mode FROM playlist WHERE playlist_id = ?");
    ph.bind(1, playlist_id);
    if (!ph.executeStep()) return std::nullopt;

    PlaylistDetail d;
    d.playlist_id = ph.getColumn(0).getString();
    d.title       = ph.getColumn(1).getString();
    d.mode        = ph.getColumn(2).getString();

    SQLite::Statement q(db_.get(), R"(
        SELECT pi.id, pi.position, pi.item_type, pi.item_id,
               CASE pi.item_type
                   WHEN 'episode' THEN s.title || ' S' || PRINTF('%02d',e.season) ||
                                       'E' || PRINTF('%02d',e.episode) || ' — ' || e.title
                   WHEN 'movie'   THEN m.title ELSE ''
               END AS title,
               CASE pi.item_type
                   WHEN 'episode' THEN e.duration_ms
                   WHEN 'movie'   THEN m.duration_ms ELSE 0
               END AS duration_ms,
               e.season, e.episode
        FROM playlist_item pi
        LEFT JOIN episode e ON pi.item_type = 'episode' AND pi.item_id = e.episode_id
        LEFT JOIN show    s ON e.show_id = s.show_id
        LEFT JOIN movie   m ON pi.item_type = 'movie'   AND pi.item_id = m.movie_id
        WHERE pi.playlist_id = ? ORDER BY pi.position
    )");
    q.bind(1, playlist_id);
    while (q.executeStep()) {
        PlaylistItemRow r;
        r.id          = q.getColumn(0).getInt64();
        r.position    = q.getColumn(1).getInt();
        r.item_type   = q.getColumn(2).getString();
        r.item_id     = q.getColumn(3).getString();
        r.title       = q.getColumn(4).getString();
        r.duration_ms = q.getColumn(5).getInt64();
        if (!q.getColumn(6).isNull()) {
            r.season  = q.getColumn(6).getInt();
            r.episode = q.getColumn(7).getInt();
        }
        d.items.push_back(std::move(r));
    }
    return d;
}

std::pair<std::string, int> PlaylistRepository::createFromBlock(const std::string& block_id,
                                                                  const std::string& title) {
    struct Item { std::string item_type; std::string item_id; };
    std::vector<Item> items;

    SQLite::Statement cq(db_.get(), R"(
        SELECT content_type, content_id, season_filter, episode_order, include_specials
        FROM block_content WHERE block_id = ? ORDER BY position
    )");
    cq.bind(1, block_id);
    while (cq.executeStep()) {
        std::string ct  = cq.getColumn(0).getString();
        std::string cid = cq.getColumn(1).getString();
        if (ct == "show") {
            bool        has_season    = !cq.getColumn(2).isNull();
            int         season_filter = has_season ? cq.getColumn(2).getInt() : -1;
            bool        incl_specials = cq.getColumn(4).getInt() != 0;
            std::string ep_order      = cq.getColumn(3).getString();
            std::string order_col     = (ep_order == "air_date") ? "air_date" : "season, episode";
            std::string where_extra   = has_season     ? " AND season = ?"
                                      : !incl_specials ? " AND season > 0"
                                      : "";
            SQLite::Statement eq(db_.get(),
                "SELECT episode_id FROM episode WHERE show_id = ?" +
                where_extra + " ORDER BY " + order_col);
            eq.bind(1, cid);
            if (has_season) eq.bind(2, season_filter);
            while (eq.executeStep())
                items.push_back({"episode", eq.getColumn(0).getString()});
        } else if (ct == "movie") {
            items.push_back({"movie", cid});
        }
    }

    std::string playlist_id = db::generateId();
    SQLite::Transaction txn(db_.get());
    SQLite::Statement ps(db_.get(),
        "INSERT INTO playlist (playlist_id, title, source, mode) VALUES (?,?,'custom','sequential')");
    ps.bind(1, playlist_id); ps.bind(2, title); ps.exec();

    int pos = 0;
    for (const auto& item : items) {
        SQLite::Statement ins(db_.get(),
            "INSERT INTO playlist_item (playlist_id, position, item_type, item_id) VALUES (?,?,?,?)");
        ins.bind(1, playlist_id); ins.bind(2, pos++);
        ins.bind(3, item.item_type); ins.bind(4, item.item_id);
        ins.exec();
    }
    txn.commit();
    return {std::move(playlist_id), static_cast<int>(items.size())};
}

void PlaylistRepository::upsertPlexLink(const std::string& list_type,
                                         const std::string& list_id,
                                         const std::string& source_id,
                                         const std::string& external_id,
                                         const std::string& plex_type) {
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    SQLite::Statement s(db_.get(), R"(
        INSERT INTO plex_list_link (list_type, list_id, source_id, external_id, plex_type, last_synced_at)
        VALUES (?,?,?,?,?,?)
        ON CONFLICT(list_type, list_id) DO UPDATE SET
            source_id      = excluded.source_id,
            external_id    = excluded.external_id,
            plex_type      = excluded.plex_type,
            last_synced_at = excluded.last_synced_at
    )");
    s.bind(1, list_type); s.bind(2, list_id); s.bind(3, source_id);
    s.bind(4, external_id); s.bind(5, plex_type); s.bind(6, now);
    s.exec();
}
