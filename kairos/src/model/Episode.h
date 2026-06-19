#pragma once
#include <optional>
#include <string>
#include <cstdint>

struct Episode {
    std::string episode_id;
    std::string show_id;
    int         season      = 0;
    int         episode     = 0;
    std::string title;
    std::string file_path;
    int64_t     duration_ms = 0;

    std::string      overview;
    std::string      air_date;       // "YYYY-MM-DD"
    std::string      thumb;
    std::optional<int> absolute_index; // TVDB absolute episode number; null if not available
};
