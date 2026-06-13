#pragma once
#include <optional>
#include <string>
#include <vector>

enum class BlockType   { Episode, Premier, Filler, Movie };
enum class Advancement { Sequential, Shuffle, RerunShuffle };
enum class CursorScope { Global, Channel, Block };

struct BlockContent {
    int                id            = 0;
    std::string        block_id;
    std::string        content_type; // "show"|"movie"|"episode"|"playlist"|"filler_list"
    std::string        content_id;
    int                position      = 0;
    std::optional<int> season_filter;
};

struct Block {
    std::string                block_id;
    std::string                channel_id;
    BlockType                  block_type         = BlockType::Episode;
    int                        day_mask           = 127; // Sun=1 Mon=2 Tue=4 Wed=8 Thu=16 Fri=32 Sat=64
    std::string                start_time         = "00:00";
    std::optional<std::string> end_time;
    int                        priority           = 0;
    int                        program_count      = 0;
    std::string                max_content_rating;
    Advancement                advancement        = Advancement::Sequential;
    CursorScope                cursor_scope       = CursorScope::Block;
    int                        late_start_mins    = 0;
    int                        align_to_mins      = 0;
    bool                       inter_filler       = false;
    int                        early_start_secs   = 0;
    std::string                filler_selection   = "round_robin";
    std::vector<BlockContent>  content;
};
