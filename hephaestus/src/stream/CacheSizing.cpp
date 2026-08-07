#include "CacheSizing.h"
#include "ChannelSession.h" // kLiveHlsSegmentSecs, kLiveCacheMaxSegmentsDefault
#include "EncoderArgs.h"    // defaultBitrateCapKbps, resolveMaxHeight

namespace CacheSizing
{
	// Floor for HEPH_SEGMENT_CACHE_MB: exactly what the default-bucket
	// group_cap (kLiveCacheMaxSegmentsDefault, see Router.cpp's segment-cache
	// wiring) will actually admit per channel, summed across every channel
	// Kairos has configured. Deliberately excludes the native/direct-stream
	// bucket and VOD/preview — those draw on the same budget opportunistically
	// above this floor and are self-limiting (LRU eviction, no hard failure)
	// rather than something that can be sized up front the way a channel's
	// admin-configured bitrate can.
	size_t suggestSegmentCacheMb(const std::vector<KairosChannel>& channels)
	{
		size_t total_bytes = 0;
		for (const auto& ch : channels)
		{
			int video_kbps = ch.stream_video_bitrate > 0
								 ? ch.stream_video_bitrate
								 : defaultBitrateCapKbps(resolveMaxHeight(ch.stream_resolution));
			int audio_kbps = ch.stream_audio_bitrate > 0 ? ch.stream_audio_bitrate : 192;

			size_t bytes_per_channel = static_cast<size_t>(video_kbps + audio_kbps) * 1000 / 8
				* static_cast<size_t>(kLiveHlsSegmentSecs) * kLiveCacheMaxSegmentsDefault;
			total_bytes += bytes_per_channel;
		}
		return (total_bytes + (1024 * 1024 - 1)) / (1024 * 1024); // round up to whole MB
	}
}