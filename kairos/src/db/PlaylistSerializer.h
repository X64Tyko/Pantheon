#pragma once
#include "BlockSerializer.h"
#include <nlohmann/json.hpp>
#include <string>

class Database;

// Portable JSON export/import for playlists — same shape/intent as
// ChannelSerializer, letting a playlist be shared or moved between Pantheon
// instances (or people) as a downloadable file. Reuses BlockSerializer's
// slot resolution (already handles "movie"/"episode" content types exactly,
// which is all a playlist item ever is) rather than duplicating it.
class PlaylistSerializer {
public:
    explicit PlaylistSerializer(Database& db);

    // Export a playlist to a portable JSON blob.
    // deep=true: includes external IDs (imdb_id, tvdb_id, tmdb_id) for lossless round-trips.
    nlohmann::json exportPlaylist(const std::string& playlist_id, bool deep = false);

    struct ImportResult {
        std::string    playlist_id;
        nlohmann::json unresolved; // array of {content_type, title, reason}
    };

    // Import a playlist from a JSON blob. Resolves items by title (and IDs if deep=true).
    // All writes are wrapped in a transaction; throws on DB error.
    ImportResult importPlaylist(const nlohmann::json& body, bool deep = false);

    // Dry-run: resolve items without writing anything.
    // Returns {items: [...], unresolved_count: N}.
    nlohmann::json previewImport(const nlohmann::json& body, bool deep = false);

private:
    Database&       db_;
    BlockSerializer slots_;
};