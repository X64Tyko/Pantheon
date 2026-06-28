#pragma once
#include <cstdint>
#include <optional>
#include <string>

struct PlexLinkRow {
    std::string source_id, external_id, plex_type;
    std::optional<int64_t> last_synced_at;
};
