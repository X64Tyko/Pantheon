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

// Smart-membership fields (Database.cpp migration 87) — a "smart" playlist's
// items are periodically recomputed from filter_expr into real playlist_item
// rows (see PlaylistRepository::refreshSmart / SyncManager::refreshSmartPlaylists)
// rather than resolved live on every read, so channel/block cursors (which
// point at playlist_item.position) never see membership shift out from
// under them mid-read the way a truly live query could.
struct SmartPlaylistFields {
    std::string membership  = "static"; // "static" | "smart"
    std::string filter_expr;
    std::string smart_type  = "movie";  // "show" | "movie"
    std::string smart_sort  = "title";
    int smart_limit = 0;                // <= 0 = unlimited
    std::optional<int64_t> last_smart_refresh_at;
};

// Home-shelf fields (Database.cpp migration 88) — a home shelf is just a
// playlist with show_on_home=1; there's no separate shelf-definition table.
// home_tile_limit is a *display* cap (how many tiles show before the
// "Continue in Library" end tile), independent of smart_limit which caps
// actual playlist membership/scheduling. home_active_start/end (MM-DD, both
// empty = always shown) gate a shelf to a date window — see
// PlaylistService.cpp's inActiveWindow for the wraparound-safe check.
struct HomeShelfFields {
    bool show_on_home = false;
    int  home_order = 0;
    int  home_tile_limit = 16;
    std::string home_active_start, home_active_end;
};

struct PlaylistRow : SmartPlaylistFields, HomeShelfFields {
    std::string playlist_id, title, mode;
    int item_count = 0;
    int64_t total_ms = 0;
    std::optional<PlexLinkRow> plex_link;
};

struct PlaylistDetail : SmartPlaylistFields, HomeShelfFields {
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

    // Home shelf definitions — every smart playlist with show_on_home=1
    // whose active window (if any) includes today, ordered by home_order.
    // Static playlists are excluded: a shelf needs a filter to hand to
    // getShows/getMovies the same way the Home page's own built-in shelves
    // already work; a static playlist's explicit item list has no such
    // representation without a separate item-resolution path.
    struct HomeShelfRow {
        std::string playlist_id, title, smart_type, filter_expr, smart_sort;
        int home_tile_limit = 16;
    };
    std::vector<HomeShelfRow> listHomeShelves();

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

    // Recomputes one smart playlist's items from its stored filter_expr and
    // writes them via replaceListItems, then stamps last_smart_refresh_at.
    // Throws if the playlist doesn't exist or isn't membership='smart'. Runs
    // synchronously on the caller's connection (the main API thread's db_) —
    // see SyncManager::refreshSmartPlaylists for the equivalent bulk/
    // background-thread pass used by the regular sync cycle.
    int refreshSmart(const std::string& playlist_id);

private:
    Database& db_;
};
