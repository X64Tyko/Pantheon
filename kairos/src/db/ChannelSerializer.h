#pragma once
#include "BlockSerializer.h"
#include <nlohmann/json.hpp>
#include <string>

class Database;

class ChannelSerializer {
public:
    explicit ChannelSerializer(Database& db);

    // Export a channel config to a portable JSON blob.
    // deep=true: includes external IDs (imdb_id, tvdb_id, tmdb_id) for lossless round-trips.
    nlohmann::json exportChannel(const std::string& channel_id, bool deep = false);

    struct ImportResult {
        std::string    channel_id;
        nlohmann::json unresolved; // array of {block_name, content_type, title, reason}
    };

    // Import a channel from a JSON blob. Resolves content by title (and IDs if deep=true).
    // All writes are wrapped in a transaction; throws on DB error.
    ImportResult importChannel(const nlohmann::json& body, bool deep = false);

    // Dry-run: resolve content without writing anything.
    // Returns {blocks: [...], unresolved_count: N}.
    nlohmann::json previewImport(const nlohmann::json& body, bool deep = false);

private:
    Database&        db_;
    BlockSerializer  blocks_;
};
