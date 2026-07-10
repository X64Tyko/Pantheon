#include "PlaylistSerializer.h"
#include "Database.h"
#include "PlaylistRepository.h"
#include <SQLiteCpp/SQLiteCpp.h>

using json = nlohmann::json;

PlaylistSerializer::PlaylistSerializer(Database& db) : db_(db), slots_(db) {}

// ── Export ────────────────────────────────────────────────────────────────────

json PlaylistSerializer::exportPlaylist(const std::string& playlist_id, bool deep) {
    SQLite::Statement pq(db_.get(), "SELECT title, mode FROM playlist WHERE playlist_id = ?");
    pq.bind(1, playlist_id);
    if (!pq.executeStep()) throw std::runtime_error("playlist not found");

    json playlist_j = {
        {"title", pq.getColumn(0).getString()},
        {"mode",  pq.getColumn(1).getString()},
    };

    SQLite::Statement iq(db_.get(), R"(
        SELECT item_type, item_id FROM playlist_item
        WHERE playlist_id = ? ORDER BY position
    )");
    iq.bind(1, playlist_id);
    json items = json::array();
    while (iq.executeStep()) {
        json slot = slots_.exportSlot(iq.getColumn(0).getString(), iq.getColumn(1).getString(), deep);
        if (!slot.is_null()) items.push_back(std::move(slot));
    }

    return json{
        {"kairos_export", 1},
        {"depth",         deep ? "deep" : "shallow"},
        {"playlist",      playlist_j},
        {"items",         items},
    };
}

// ── Preview ───────────────────────────────────────────────────────────────────

json PlaylistSerializer::previewImport(const json& body, bool deep) {
    int  unresolved_count = 0;
    json preview_items    = json::array();

    for (const auto& item : body.value("items", json::array())) {
        std::string ct = item.value("content_type", "");
        bool resolved  = !slots_.resolveSlot(item, deep).empty();
        if (!resolved) ++unresolved_count;

        json out = {{"content_type", ct}, {"title", item.value("title", "")}, {"resolved", resolved}};
        for (const char* k : {"year", "season", "episode"})
            if (item.contains(k) && !item[k].is_null()) out[k] = item[k];
        preview_items.push_back(out);
    }
    return json{{"items", preview_items}, {"unresolved_count", unresolved_count}};
}

// ── Import ────────────────────────────────────────────────────────────────────

PlaylistSerializer::ImportResult PlaylistSerializer::importPlaylist(const json& body, bool deep) {
    const json& pl = body.at("playlist");

    // No enclosing transaction here, deliberately: PlaylistRepository's
    // methods (unlike BlockSerializer's) are designed for standalone
    // route-level use and already self-transact internally (addItems()
    // wraps its own SQLite::Transaction) — wrapping this whole function in
    // another one throws "cannot start a transaction within a transaction".
    // create() is a single atomic INSERT on its own, so this is still safe;
    // it's just two separate atomic steps rather than one combined unit.
    PlaylistRepository repo(db_);
    std::string mode = pl.value("mode", "sequential");
    if (mode != "sequential" && mode != "show_collection") mode = "sequential";
    std::string playlist_id = repo.create(pl.value("title", "Imported Playlist"), mode);

    json unresolved = json::array();
    std::vector<std::pair<std::string, std::string>> resolved_items;
    for (const auto& item : body.value("items", json::array())) {
        std::string ct  = item.value("content_type", "");
        std::string cid = slots_.resolveSlot(item, deep);
        if (cid.empty()) {
            unresolved.push_back({
                {"content_type", ct},
                {"title",        item.value("title", "")},
                {"reason",       "not found in library"},
            });
            continue;
        }
        resolved_items.emplace_back(ct, cid);
    }
    repo.addItems(playlist_id, resolved_items);

    return {playlist_id, unresolved};
}
