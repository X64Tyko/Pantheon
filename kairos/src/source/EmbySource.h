#pragma once
#include "JellyfinBaseSource.h"
#include <string>

// Emby uses the same REST API shape as Jellyfin; the only runtime difference
// is the auth header name and the sourceType() string.
class EmbySource final : public JellyfinBaseSource {
public:
    EmbySource(const std::string& source_id,
               const std::string& base_url,
               const std::string& token,
               const std::string& user_id)
        : JellyfinBaseSource(source_id, base_url, token, user_id, "X-Emby-Token") {}

    std::string sourceId()   const override { return source_id_; }
    std::string sourceType() const override { return "emby"; }
};
