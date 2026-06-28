#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "../model/Block.h"
#include <nlohmann/json.hpp>

class Database;

class BlockRepository {
public:
    explicit BlockRepository(Database& db);

    // Load all blocks for a channel, fully populated with content + filler_entries.
    std::vector<Block> loadBlocks(const std::string& channel_id);

    // Load a single block by block_id, fully populated. Returns nullopt if not found.
    std::optional<Block> loadBlock(const std::string& block_id);

    // Channel scheduling metadata.
    std::string channelTimezone(const std::string& channel_id);
    std::string channelAdvanceMode(const std::string& channel_id);

    // media_cursor R/W (show/playlist/filler_list sequential position).
    int  readCursorPos(const std::string& content_type, const std::string& content_id,
                       const std::string& scope, const std::string& scope_id);
    void writeCursorPos(const std::string& content_type, const std::string& content_id,
                        const std::string& scope, const std::string& scope_id,
                        int pos, const std::string& episode_id = "");

    // block_state: round-robin content position.
    int  readBlockRR(const std::string& block_id, const std::string& channel_id);
    void writeBlockRR(const std::string& block_id, const std::string& channel_id, int pos);

    // block_state: rerun runs_remaining + consecutive_count.
    int  readRunsRemaining(const std::string& block_id, const std::string& channel_id);
    int  readConsecutiveCount(const std::string& block_id, const std::string& channel_id);
    void writeRerunState(const std::string& block_id, const std::string& channel_id,
                         int content_pos, int runs_remaining, int consecutive_count = 0);

    // Returns full block list JSON array for a channel (content items + filler entries included).
    nlohmann::json listWithContent(const std::string& channel_id);

    // ── Block CRUD ────────────────────────────────────────────────────────────
    std::string createBlock(const std::string& channel_id, const nlohmann::json& params);
    void        updateBlockField(const std::string& block_id, const std::string& col, const std::string& val);
    void        updateBlockField(const std::string& block_id, const std::string& col, int val);
    void        clearBlockField(const std::string& block_id, const std::string& col);
    void        removeBlock(const std::string& block_id);

    // ── block_content CRUD ────────────────────────────────────────────────────
    // Returns {rowid, position}.
    std::pair<int64_t, int> addContent(const std::string& block_id, const nlohmann::json& params);
    void updateContentField(int id, const std::string& col, const std::string& val);
    void updateContentField(int id, const std::string& col, int val);
    void clearContentField(int id, const std::string& col);
    void removeContent(int id);
    // Resets the media_cursor for a show content item.
    // Throws std::invalid_argument if the item is not a show type.
    // Throws std::runtime_error if the content row or block is not found.
    void resetContentCursor(const std::string& channel_id, const std::string& block_id, int content_row_id);

    // ── block_filler_entry CRUD ───────────────────────────────────────────────
    struct FillerEntryResult { int64_t id; int position; std::string title; };
    FillerEntryResult addFillerEntry(const std::string& block_id, const nlohmann::json& params);
    void updateFillerEntryField(int id, const std::string& col, const std::string& val);
    void updateFillerEntryField(int id, const std::string& col, int val);
    void removeFillerEntry(int id);

    // ── channel_bumper CRUD ───────────────────────────────────────────────────
    struct BumperRow {
        int         id;
        std::string content_type;
        std::string content_id;
        std::string mode;
        int         every_n;
        int         position;
        std::string title;
        std::optional<int> season_filter;
    };
    std::vector<BumperRow> listBumpers(const std::string& channel_id);

    struct BumperResult { int id; int position; std::string title; };
    BumperResult addBumper(const std::string& channel_id,
                            const std::string& content_type, const std::string& content_id,
                            const std::string& mode, int every_n,
                            std::optional<int> season_filter = std::nullopt);
    void updateBumperField(int id, const std::string& col, const std::string& val);
    void updateBumperField(int id, const std::string& col, int val);
    void removeBumper(int id);

    // ── Episode group CRUD ────────────────────────────────────────────────────
    std::string createEpisodeGroup(const std::string& show_id, const std::string& name,
                                    const std::string& group_type);
    void removeEpisodeGroup(const std::string& group_id);
    // Returns {rowid, part_num}.
    std::pair<int64_t, int> addGroupMember(const std::string& group_id,
                                            const std::string& episode_id, int part_num);
    void removeGroupMember(int member_id);

private:
    Database& db_;
};
