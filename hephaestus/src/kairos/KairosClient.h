#pragma once
#include "KairosTypes.h"
#include <string>
#include <optional>
#include <vector>

class KairosClient
{
	std::string base_url;
	std::string internal_token_conf_path;

public:
	explicit KairosClient(std::string base_url, std::string internal_token_conf_path = "");

	// atMs == -1 → omit the ?at= parameter (use server-side wall clock)
	std::optional<KairosNowResponse> getNow(const std::string& channelId, int64_t atMs = -1);

	// Looks ahead to the item scheduled to air after whatever's currently
	// playing — Kairos's /next (distinct from /now), used by ChannelSession's
	// prefetch loop to warm Hephaestus's own file probe cache before a
	// transition actually needs it. Reuses KairosNowResponse's shape; fields
	// /next doesn't return (wall_clock_end_ms, is_filler, keyframes_*) stay
	// at their defaults — callers needing those should go through getNow().
	std::optional<KairosNowResponse> getNext(const std::string& channelId);

	// Fire-and-forget; logs on failure but never throws.
	void markPlayed(const std::string& channelId,
					const std::string& itemType,
					const std::string& itemId,
					const std::string& blockId,
					int64_t durationActualMs);

	std::vector<KairosChannel> getChannels();

	// Resolves a library item (movie/episode) to a playable file for VOD.
	// contentType must be "movie" or "episode". bearerToken, when non-empty,
	// is forwarded as Authorization so Kairos can resolve currentUser() and
	// include this user's sticky per-show track preference in the response.
	// partNum > 0 requests a specific part of a multi-part movie (GitHub #3)
	// directly, forwarded as Kairos's ?part= query param; 0 lets Kairos pick
	// (defaults to part 1). Ignored entirely for non-multi-part items.
	std::optional<PlaybackInfo> getPlaybackInfo(const std::string& contentType,
												const std::string& contentId,
												const std::string& bearerToken = "",
												int partNum                    = 0);

	// Fire-and-forget (like markPlayed): persists a fresh keyframe probe back
	// into Kairos so the next direct-stream session on this file skips its
	// own ffprobe scan too — see VodSession::computeSegmentBoundaries()'s own
	// fallback-probe path, the only caller. contentType/contentId are the
	// same pair passed to getPlaybackInfo(), not necessarily what Kairos ends
	// up writing to (a linked special resolves through its movie server-side).
	void pushKeyframeCache(const std::string& contentType,
						   const std::string& contentId,
						   const std::vector<int64_t>& keyframesMs,
						   int64_t size,
						   int64_t mtime);

	// Fetches the live stream_buffer_size from Kairos's /api/config/settings,
	// in KB (matching the Hades UI unit) — callers must convert to bytes.
	// Returns nullopt on any failure so callers can fall back to a local default.
	std::optional<int> getBufferSize();

	// Same /api/config/settings blob, plucking verbose_transcode_logs instead
	// — lets operators flip on full ffmpeg command/-v verbose logging from
	// the Hades settings page without restarting Hephaestus.
	std::optional<bool> getVerboseTranscodeLogs();

	// Same blob again, plucking ffmpeg_debug_logs — was HEPH_FFMPEG_DEBUG, an
	// env var fixed at startup, until this; same "no restart needed" treatment
	// as getVerboseTranscodeLogs(), and controls a genuinely independent
	// thing (whether ffmpeg's stderr is forwarded live at all, vs just
	// tail-captured for an on-failure dump) — see FfmpegProcess's log_stderr.
	std::optional<bool> getFfmpegDebugLogs();
};