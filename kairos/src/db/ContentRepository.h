#pragma once
#include <cstdint>
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../model/Episode.h"
#include "../model/Movie.h"

class Database;

// Items loaded for filler/picker selection (type, id, duration).
struct FillerItem {
    std::string item_type;  // "episode" | "movie"
    std::string item_id;
    int64_t     duration_ms = 0;
};

class ContentRepository {
public:
    explicit ContentRepository(Database& db);

    // ── Episodes ──────────────────────────────────────────────────────────────

    std::vector<Episode> getEpisodes(const std::string& show_id,
                                     std::optional<int> season = std::nullopt,
                                     bool include_specials = false,
                                     const std::string& episode_order = "season");

    // Episodes with play history (before_time) in the given channel scope.
    std::vector<Episode> getPlayedEpisodes(const std::string& show_id,
                                           const std::string& channel_id,
                                           std::optional<int> season,
                                           std::time_t before_time,
                                           bool global_scope = false,
                                           bool include_specials = false,
                                           const std::string& episode_order = "season");

    // Like getPlayedEpisodes but trims the most-recently-played smart_pct% from the pool.
    std::vector<Episode> getPlayedEpisodesWithCooldown(const std::string& show_id,
                                                       const std::string& channel_id,
                                                       std::optional<int> season,
                                                       int smart_pct,
                                                       std::time_t before_time,
                                                       bool global_scope = false,
                                                       bool include_specials = false);

    // ── Movies ────────────────────────────────────────────────────────────────

    std::optional<Movie> getMovie(const std::string& movie_id);

    // ── Lists (playlist / filler_list) ───────────────────────────────────────

    std::vector<std::pair<std::string, std::string>>
        loadListItems(const std::string& content_type, const std::string& content_id);

    // Filler items with duration, handling filler_list/playlist/show/movie content types.
    std::vector<FillerItem> loadFillerItems(const std::string& content_type,
                                             const std::string& content_id,
                                             std::optional<int> season_filter = std::nullopt);

    // ── Playlist show_collection helpers ─────────────────────────────────────

    std::string              getPlaylistMode(const std::string& playlist_id);
    std::vector<std::string> getPlaylistShows(const std::string& playlist_id);
    std::vector<Episode>     getPlaylistShowEpisodes(const std::string& playlist_id,
                                                      const std::string& show_id);

    // Number of items in a playlist (for cursor wrap-around).
    int getPlaylistItemCount(const std::string& playlist_id);

    // ── Metadata helpers ──────────────────────────────────────────────────────

    std::string showTitle(const std::string& show_id);

    // ── Episode group membership ──────────────────────────────────────────────

    // Returns ep_id → {group_id, part_num} for all episodes belonging to a group in show.
    std::unordered_map<std::string, std::pair<std::string, int>>
        getEpisodeGroupMap(const std::string& show_id);

    // If episode_id is a mid-group part (part_num > 1), returns the Part 1 episode_id.
    std::optional<std::string> findGroupPart1(const std::string& episode_id);

    // ── Play-history hot-ID queries ───────────────────────────────────────────

    // Most recently played movie IDs (hot set for SmartShuffle cooldown).
    std::unordered_set<std::string> getHotMovieIds(const std::string& channel_id,
                                                    std::time_t before_time,
                                                    int limit);

    // Most recently played episode IDs for a given show (hot set for SmartShuffle).
    std::unordered_set<std::string> getHotEpisodeIds(const std::string& channel_id,
                                                      std::time_t before_time,
                                                      const std::string& show_id,
                                                      int limit);

    // Recency map: item_id → last aired_at (for "sized" filler selection).
    std::unordered_map<std::string, int64_t> getLastPlayedMap(const std::string& channel_id,
                                                               std::time_t before_time);

private:
    Database& db_;

    static Episode rowToEpisode(SQLite::Statement& q);
};
