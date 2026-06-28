#pragma once
#include "JellyfinBaseSource.h"
#include <string>

class JellyfinSource final : public JellyfinBaseSource {
public:
    JellyfinSource(const std::string& source_id,
                   const std::string& base_url,
                   const std::string& token,
                   const std::string& user_id)
        : JellyfinBaseSource(source_id, base_url, token, user_id, "X-MediaBrowser-Token") {}

    std::string sourceId()   const override { return source_id_; }
    std::string sourceType() const override { return "jellyfin"; }
};
