#pragma once
#include "KairosTypes.h"
#include <string>
#include <optional>
#include <vector>

class KairosClient {
    std::string base_url;
public:
    explicit KairosClient(std::string base_url);

    // atMs == -1 → omit the ?at= parameter (use server-side wall clock)
    std::optional<KairosNowResponse> getNow(const std::string& channelId, int64_t atMs = -1);

    // Fire-and-forget; logs on failure but never throws.
    void markPlayed(const std::string& channelId,
                    const std::string& itemType,
                    const std::string& itemId,
                    const std::string& blockId,
                    int64_t durationActualMs);

    std::vector<KairosChannel> getChannels();

    // Fetches the live stream_buffer_size from Kairos's /api/config/settings.
    // Returns nullopt on any failure so callers can fall back to a local default.
    std::optional<int> getBufferSize();
};
