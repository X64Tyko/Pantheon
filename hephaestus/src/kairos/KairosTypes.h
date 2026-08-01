#pragma once
#include "model/ExternalSubtitle.h"
#include <string>
#include <optional>
#include <cstdint>
#include <vector>

struct KairosNowResponse
{
	std::string item_type;
	std::string item_id;
	std::string file_path;
	std::string title;
	std::string block_id;
	int64_t duration_ms         = 0;
	int64_t wall_clock_start_ms = 0;
	int64_t wall_clock_end_ms   = 0;
	bool is_filler              = false;
	std::optional<std::string> show_title;
	std::optional<std::string> show_id;
	std::optional<std::string> source_id;
	std::optional<std::string> external_id;
	std::optional<int> season;
	std::optional<int> episode_num;
	std::optional<std::string> offline_image_path;
	std::optional<std::string> offline_audio_path;
	// Cached direct-stream keyframe data from Kairos's own sync-time probe
	// (Database.cpp's v98 migration) — same data PlaybackInfo::keyframes_ms
	// already gives VOD sessions, exposed here too so ChannelSession can snap
	// a direct-stream item's start offset to a real keyframe instead of
	// handing ffmpeg a blind offset on every transition. Empty keyframes_ms
	// means "not cached yet" (item_type isn't movie/episode, or Kairos
	// hasn't sync-probed it) — callers must fall back to the raw offset.
	std::vector<int64_t> keyframes_ms;
	int64_t keyframes_size  = 0;
	int64_t keyframes_mtime = 0;
};

// Resolved from Kairos's internal /api/playback/:content_type/:id endpoint
// when starting a VOD session.
struct PlaybackInfo
{
	std::string file_path;
	std::string title;
	int64_t duration_ms = 0;
	std::vector<ExternalSubtitle> external_subtitles;
	// Sticky per-show track preference (Plex-style) — only populated when the
	// original caller's bearer token was forwarded to this call. Empty when
	// absent or when the item has no show (e.g. a movie).
	std::string preferred_audio_lang;
	std::string preferred_subtitle_lang;
	// Cached direct-stream keyframe data from Kairos's own sync-time probe
	// (Database.cpp's v98 migration) — VodSession::computeSegmentBoundaries()
	// uses this instead of running its own full-file ffprobe scan when it's
	// non-empty and keyframes_size/keyframes_mtime still match the file's
	// current stat(). Empty keyframes_ms means "not cached yet."
	std::vector<int64_t> keyframes_ms;
	int64_t keyframes_size  = 0;
	int64_t keyframes_mtime = 0;
};

struct KairosChannel
{
	std::string channel_id;
	std::string name;
	int number = 0;
	std::string audio_lang;    // overrides global --audio-lang when non-empty
	std::string subtitle_lang; // empty = no subtitle mapping
	std::string logo_path;     // empty = no channel-specific logo configured
	// Per-channel transcode quality (mirrors channel.stream_* DB columns)
	std::string stream_resolution = "source"; // "source"|"1080p"|"720p"|"480p"
	int stream_video_bitrate      = 0;        // kbps; 0 = CRF/CQ auto
	int stream_audio_bitrate      = 192;      // kbps
};