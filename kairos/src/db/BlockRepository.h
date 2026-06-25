#pragma once
#include <ctime>
#include <string>
#include <vector>
#include "../model/Block.h"

class Database;

class BlockRepository {
public:
    explicit BlockRepository(Database& db);

    // Load all blocks for a channel, fully populated with content + filler_entries.
    std::vector<Block> loadBlocks(const std::string& channel_id);

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

private:
    Database& db_;
};
