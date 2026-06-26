#include "PlaylistRepository.h"
#include "Database.h"
#include "DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>

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
