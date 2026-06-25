#pragma once
#include <nlohmann/json.hpp>
#include <string>

class Database;

class BlockSerializer {
public:
    explicit BlockSerializer(Database& db);

    // Resolve a portable slot JSON {content_type, title, [imdb_id, ...]} → local content_id.
    // Public so ChannelSerializer can use it for bumper resolution.
    std::string resolveSlot(const nlohmann::json& slot, bool deep);

    // Convert a local content_type + content_id pair to a portable slot JSON with title/IDs.
    // Public so ChannelSerializer can use it for bumper export.
    nlohmann::json exportSlot(const std::string& ct, const std::string& cid, bool deep);

    // Export all fields, content items, and filler entries for one block.
    nlohmann::json exportBlock(const std::string& block_id, bool deep);

    // Insert one block (fields + content + filler + bumper slots) under channel_id.
    // Unresolved content items are appended to `unresolved`.
    // Returns the new block_id.
    std::string importBlock(const std::string& channel_id,
                            const nlohmann::json& blk,
                            bool deep,
                            nlohmann::json& unresolved);

private:
    Database& db_;
};
