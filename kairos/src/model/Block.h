#pragma once
#include <optional>
#include <string>
#include <vector>

enum class BlockType   { Episode, Premier, Filler, Movie };
enum class Advancement { Sequential, Shuffle, SmartShuffle, RerunShuffle, RerunSmart };
enum class CursorScope { Global, Channel, Block };

// What to do when a show in a rerun block has no play history on this channel.
enum class NoHistoryBehavior {
    Normal,      // play as a regular show: all episodes, sequential show cursor
    FallbackAll, // treat the full episode catalog as the rerun pool (keep rerun advancement)
    Exclude,     // exclude from weighted selection until the show has play history
    Skip,        // skip the slot entirely (nullopt → dead air / gap)
};

// String → enum parsers (inline so both BlockRepository and RuleEngine can use them).
inline BlockType parseBlockType(const std::string& s) {
    if (s == "premier") return BlockType::Premier;
    if (s == "filler")  return BlockType::Filler;
    if (s == "movie")   return BlockType::Movie;
    return BlockType::Episode;
}
inline Advancement parseAdvancement(const std::string& s) {
    if (s == "shuffle")       return Advancement::Shuffle;
    if (s == "smart_shuffle") return Advancement::SmartShuffle;
    if (s == "rerun_shuffle") return Advancement::RerunShuffle;
    if (s == "rerun_smart")   return Advancement::RerunSmart;
    return Advancement::Sequential;
}
inline CursorScope parseCursorScope(const std::string& s) {
    if (s == "global")  return CursorScope::Global;
    if (s == "channel") return CursorScope::Channel;
    return CursorScope::Block;
}
inline NoHistoryBehavior parseNoHistoryBehavior(const std::string& s) {
    if (s == "fallback_all") return NoHistoryBehavior::FallbackAll;
    if (s == "exclude")      return NoHistoryBehavior::Exclude;
    if (s == "skip")         return NoHistoryBehavior::Skip;
    return NoHistoryBehavior::Normal;
}

struct BlockFillerEntry {
    std::string        content_type; // "filler_list" | "playlist" | "show" | "movie"
    std::string        content_id;
    std::string        advancement   = "sized"; // "sequential" | "shuffle" | "sized"
    int                weight        = 1;
    std::optional<int> season_filter;           // null = all seasons; N = season N only
};

struct BlockContent {
    int                id              = 0;
    std::string        block_id;
    std::string        content_type;   // "show"|"movie"|"episode"|"playlist"|"filler_list"
    std::string        content_id;
    int                position        = 0;
    std::optional<int> season_filter;
    int                weight          = 1;    // weighted show selection (rerun modes)
    int                run_count       = 1;    // sequential episodes per selection (rerun modes)
    bool               include_specials = false; // include season 0 episodes
    std::string        episode_order   = "season"; // "season" | "absolute" | "airdate"
};

struct ChannelBumper {
    int         id           = 0;
    std::string channel_id;
    std::string content_type; // "show" | "episode" | "playlist"
    std::string content_id;
    std::string mode         = "between"; // "between" | "filler"
    int         every_n      = 3;
    int         position     = 0;
};

struct Block {
    std::string                block_id;
    std::string                channel_id;
    BlockType                  block_type          = BlockType::Episode;
    int                        day_mask            = 127; // Sun=1 Mon=2 Tue=4 Wed=8 Thu=16 Fri=32 Sat=64
    std::string                start_time          = "00:00";
    std::optional<std::string> end_time;
    int                        priority            = 0;
    int                        program_count       = 0;
    std::string                max_content_rating;
    Advancement                advancement         = Advancement::Sequential;
    CursorScope                cursor_scope        = CursorScope::Block;
    int                        late_start_mins     = 0;
    int                        align_to_mins       = 0;
    bool                       inter_filler        = false;
    int                        early_start_secs    = 0;
    std::string                filler_selection    = "round_robin";
    int                        smart_pct                  = 30; // cooldown threshold % for smart modes
    int                        max_consecutive_episodes   = 0;  // 0 = unlimited
    std::string                start_scope         = "block"; // "block" | "episode"
    NoHistoryBehavior          no_history_behavior = NoHistoryBehavior::Normal;
    bool                       snap_to_group_start = true; // snap mid-group random picks to Part 1
    // Intro: plays once at block start (before first content item).
    std::string                intro_content_type;
    std::string                intro_content_id;
    // Outro: plays once when program_count is hit (after last content item).
    std::string                outro_content_type;
    std::string                outro_content_id;
    // Interstitial: plays between show transitions at configurable frequency (0=disabled).
    std::string                interstitial_content_type;
    std::string                interstitial_content_id;
    int                        interstitial_every_n = 1;
    std::vector<BlockContent>  content;
    std::vector<BlockFillerEntry> filler_entries; // empty = inherit channel default
};
