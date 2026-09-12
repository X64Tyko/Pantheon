#pragma once
#include "../kairos/KairosTypes.h"
#include <vector>

// Computes a startup-time suggestion for HEPH_SEGMENT_CACHE_MB from the
// deployment's actual configured channels — not a guessed default. See
// suggestSegmentCacheMb's own comment for the formula and main.cpp for
// where the resulting number gets logged.
namespace CacheSizing
{
	size_t suggestSegmentCacheMb(const std::vector<KairosChannel>& channels);
}