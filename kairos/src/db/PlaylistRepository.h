#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class Database;

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

    // Expand block content to episodes/movies, create playlist + items in a transaction.
    // Returns {playlist_id, item_count}.
    std::pair<std::string, int> createFromBlock(const std::string& block_id,
                                                 const std::string& title);

private:
    Database& db_;
};
