#pragma once
#include "Episode.h"
#include <string>
#include <cstdint>

struct ProgramEntry {
    Episode     episode;
    std::string channel_id;
    std::string block_id;
    int64_t     wall_clock_start_ms = 0; // unix epoch ms
    int64_t     wall_clock_end_ms   = 0;
};
