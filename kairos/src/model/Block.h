#pragma once
#include <optional>
#include <string>
#include <vector>

enum class BlockType   { Episode, Premier, Filler, Movie };
enum class Advancement { Sequential, Shuffle, RerunShuffle };
enum class CursorScope { Global, Channel, Block };

struct BlockContent {
    int         id           = 0;
    std::string block_id;
    std::string content_type; // "show" | "movie" | "playlist"
    std::string content_id;   // show_id, movie_id, or playlist_id
    int         position     = 0;
    Advancement advancement  = Advancement::Sequential;
    CursorScope cursor_scope = CursorScope::Block;
};

struct Block {
    std::string              block_id;
    std::string              channel_id;
    BlockType                block_type         = BlockType::Episode;
    int                      day_mask           = 127; // Sun=1 Mon=2 Tue=4 Wed=8 Thu=16 Fri=32 Sat=64
    std::string              start_time         = "00:00";
    std::optional<std::string> end_time;               // nullopt = fill to end of day
    int                      priority           = 0;
    std::string              max_content_rating;       // empty = no filter
    std::string              config_json        = "{}";
    std::vector<BlockContent> content;                 // populated on load
};
