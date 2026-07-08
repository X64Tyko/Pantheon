#pragma once
#include "KairosTypes.h"
#include <string>
#include <vector>

class KairosClient {
    std::string base_url_;
public:
    explicit KairosClient(std::string base_url);
    std::vector<KairosChannel> getChannels();
};
