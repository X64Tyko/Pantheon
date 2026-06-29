#pragma once
#include <string>
#include <optional>
#include <cstdint>

struct KairosNowResponse {
    std::string item_type;
    std::string item_id;
    std::string file_path;
    std::string title;
    std::string block_id;
    int64_t     duration_ms          = 0;
    int64_t     wall_clock_start_ms  = 0;
    int64_t     wall_clock_end_ms    = 0;
    bool        is_filler            = false;
    std::optional<std::string> show_title;
    std::optional<std::string> show_id;
    std::optional<std::string> source_id;
    std::optional<std::string> external_id;
    std::optional<int> season;
    std::optional<int> episode_num;
};

struct KairosChannel {
    std::string channel_id;
    std::string name;
    int         number = 0;
};
