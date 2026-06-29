#pragma once
#include <string>
#include <optional>
#include <cstdint>

struct KairosChannel {
    std::string channel_id;
    std::string name;
    int         number = 0;
};
