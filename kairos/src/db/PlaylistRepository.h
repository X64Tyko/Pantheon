#pragma once
#include "ListTypes.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Database;

struct PlaylistItemRow {
    int64_t id = 0;
    int position = 0;
    std::string item_type, item_id, title;
    int64_t duration_ms = 0;
    std::optional<int> season, episode;
};

struct PlaylistRow {
    std::string playlist_id, title, mode;
    int item_count = 0;
    int64_t total_ms = 0;
    std::optional<PlexLinkRow> plex_link;
};

struct PlaylistDetail {
    std::string playlist_id, title, mode;
    std::vector<PlaylistItemRow> items;
};

class PlaylistRepository {
public:
    explicit PlaylistRepository(Database& db);

    // Create a new empty playlist; returns playlist_id.
    std::string create(const std::string& title, const std::string& mode = "sequential");

    // Single-field string update.
    void updateField(const std::string& playlist_id, const std::string& col,
                     const std::string& val);

    void remove(const std::string& playlist_id);

    // Remove the plex_list_link record; items are kept.
    void unlinkPlex(const std::string& playlist_id);

    std::vector<PlaylistRow>      listAll();
    std::optional<PlaylistDetail> getDetail(const std::string& playlist_id);

    // Add one item; returns {rowid, position}.
    std::pair<int64_t, int> addItem(const std::string& playlist_id,
                                     const std::string& item_type,
                                     const std::string& item_id);

    // Bulk-add items in a single transaction; returns count added.
    int addItems(const std::string& playlist_id,
                 const std::vector<std::pair<std::string, std::string>>& items);

    // Remove item and renumber subsequent positions.
    void removeItem(int item_id, const std::string& playlist_id);

    // Move item to new_position (simple position update).
    void moveItem(int item_id, const std::string& playlist_id, int new_position);

    // Expand block content entries to flat {item_type, item_id} pairs.
    std::vector<std::pair<std::string, std::string>>
        expandBlockItems(const std::string& block_id);

    // Expand block content to episodes/movies, create playlist + items in a transaction.
    // Returns {playlist_id, item_count}.
    std::pair<std::string, int> createFromBlock(const std::string& block_id,
                                                 const std::string& title);

    void upsertPlexLink(const std::string& list_type, const std::string& list_id,
                        const std::string& source_id, const std::string& external_id,
                        const std::string& plex_type);

    // Replace all items in a list (playlist or filler_list) with the given set.
    // list_type must be "playlist" or "filler_list".
    void replaceListItems(const std::string& list_type,
                          const std::string& list_id,
                          const std::vector<std::pair<std::string, std::string>>& items);

private:
    Database& db_;
};
