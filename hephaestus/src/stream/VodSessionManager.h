#pragma once
#include "VodSession.h"
#include "../kairos/KairosClient.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

// Identifies a shareable video VodEncodeStream — two viewers land on the
// SAME stream iff every field here matches, including the *resolved*
// transcode decision (video_codec/max_height), not just the raw inputs that
// fed it: two clients with different declared capabilities that both
// resolve to "hevc, uncapped" must still share, and two that both declare
// hevc support but differ only in a resolution cap must NOT. video_codec is
// empty for direct-stream (no encode decision made at all).
struct VideoStreamKey
{
	std::string content_id;
	bool direct_stream;
	bool hdr_capable;
	int burn_in_track;       // -1 = none
	std::string video_codec; // resolved target, "" when direct_stream
	int max_height;          // 0 = uncapped

	bool operator<(const VideoStreamKey& o) const
	{
		return std::tie(content_id, direct_stream, hdr_capable, burn_in_track, video_codec, max_height)
			< std::tie(o.content_id, o.direct_stream, o.hdr_capable, o.burn_in_track, o.video_codec, o.max_height);
	}
};

// Same idea for audio — content_id + track + the resolved decision
// (direct-stream, or which transcode codec) is what has to match.
struct AudioStreamKey
{
	std::string content_id;
	int audio_track;
	bool direct_stream;
	std::string audio_codec; // resolved transcode target, "" when direct_stream

	bool operator<(const AudioStreamKey& o) const
	{
		return std::tie(content_id, audio_track, direct_stream, audio_codec)
			< std::tie(o.content_id, o.audio_track, o.direct_stream, o.audio_codec);
	}
};

// Keyed by generated session_id for the per-viewer VodSession facade — each
// viewer still gets their own session_id/manifest, unchanged from before.
// What's new: the underlying video/audio VodEncodeStreams a VodSession
// composes are no longer its own private unique_ptrs, but shared_ptrs handed
// out by getOrCreateVideoStream()/getOrCreateAudioStream() below, keyed by
// content+resolved-decision (VideoStreamKey/AudioStreamKey) — a second
// viewer whose key matches an already-running stream attaches to it
// (ordinary shared_ptr refcounting; no "join" protocol needed), while a
// track switch or a viewer whose resolved decision differs still gets (or
// shares) a different stream. Stored as weak_ptr so a stream nobody
// references anymore tears itself down exactly like a session already did.
class VodSessionManager
{
public:
	// max_sessions caps concurrent transcode sessions host-wide — 0 means no
	// cap. Without this, any authenticated caller (a free self-service guest
	// account included) could loop-request VOD sessions and exhaust the
	// host's CPU/GPU; see Config.h's max_vod_sessions for the default.
	VodSessionManager(std::string ffmpeg_path, VodStreamOptions opts, KairosClient& kairos,
					  int max_sessions = 0);
	~VodSessionManager();

	// Creates and starts a new per-viewer session (attaching to shared
	// video/audio streams internally — see VodSession::start()). Returns
	// nullptr if start() fails (probe failure, no duration available, or a
	// stream wouldn't spawn). content_type/content_id are Kairos's own
	// identity for this item — the sharing key's basis, and available to the
	// caller (Router.cpp) before file_path is even resolved. hdr_capable is
	// the requesting client's own display capability, client_caps its
	// declared decode capability if any (see VodSession::start).
	// fallback_duration_ms is Kairos's own library-scan duration, used only
	// if this file's own ffprobe run doesn't report one.
	// preferred_audio_lang/preferred_subtitle_lang are the caller's saved
	// per-show preference (Kairos), used only when audio_track/subtitle_track
	// are left unset (-1) by the client itself.
	std::shared_ptr<VodSession> create(const std::string& content_type, const std::string& content_id,
									   const std::string& file_path, int64_t position_ms,
									   int audio_track, int subtitle_track, bool hdr_capable,
									   const std::optional<ClientCapabilities>& client_caps,
									   const std::vector<ExternalSubtitle>& external_subtitles,
									   int64_t fallback_duration_ms,
									   const std::string& preferred_audio_lang         = "",
									   const std::string& preferred_subtitle_lang      = "",
									   const std::vector<int64_t>& kairos_keyframes_ms = {},
									   int64_t kairos_keyframes_size                   = 0,
									   int64_t kairos_keyframes_mtime                  = 0);
	std::shared_ptr<VodSession> get(const std::string& sessionId);
	void stop(const std::string& sessionId);

	// Snapshot of currently-active sessions, for the activity/debugging view
	// (ActivityRouter).
	std::vector<std::shared_ptr<VodSession>> listActive();

	// Called by VodSession itself (start()/rebuildAudioStream()) — returns
	// the existing stream for `key` if one's still alive, else constructs a
	// fresh one via `factory` (only invoked on an actual cache miss).
	std::shared_ptr<VodEncodeStream> getOrCreateVideoStream(
		const VideoStreamKey& key, const std::function<std::shared_ptr<VodEncodeStream>()>& factory);
	std::shared_ptr<VodEncodeStream> getOrCreateAudioStream(
		const AudioStreamKey& key, const std::function<std::shared_ptr<VodEncodeStream>()>& factory);

	// Thin forward to kairos.pushKeyframeCache() — kairos itself is private,
	// this is the one piece of it VodSession's own fallback-probe path (see
	// computeSegmentBoundaries()) needs to reach, the same way it already
	// reaches getOrCreateVideoStream/getOrCreateAudioStream above rather than
	// touching this manager's internals directly.
	void pushKeyframeCache(const std::string& content_type, const std::string& content_id,
						   const std::vector<int64_t>& keyframes_ms, int64_t size, int64_t mtime);

private:
	std::string ffmpeg_path;
	VodStreamOptions opts;
	KairosClient& kairos;

	std::mutex mtx;
	std::map<std::string, std::shared_ptr<VodSession>> sessions;
	int max_sessions = 0;

	std::mutex stream_mtx;
	std::map<VideoStreamKey, std::weak_ptr<VodEncodeStream>> video_streams;
	std::map<AudioStreamKey, std::weak_ptr<VodEncodeStream>> audio_streams;

	// HLS is poll-based (no persistent connection to react to), so idle
	// sessions are swept periodically rather than torn down on disconnect —
	// same reasoning as ChannelSession's hlsWatchLoop. Also sweeps expired
	// weak_ptr entries out of video_streams/audio_streams so those maps
	// don't grow unbounded over a long-running instance's lifetime.
	std::atomic<bool> stop_reaper{false};
	std::thread reaper_thread;
	void reapLoop();

	// Mirrors SessionManager's refreshCache()/kCacheRefreshInterval: applies
	// Kairos-driven settings live (currently verbose_transcode_logs and
	// buffer_size, the two global — not per-channel — settings that also
	// apply to VOD) without needing a Hephaestus restart. VOD has no
	// per-channel config the way live channels do, so there's no channel
	// list to cache here, just these two scalars.
	std::mutex settings_mtx;
	std::optional<bool> cached_verbose_transcode_logs;
	std::optional<bool> cached_ffmpeg_debug_logs;
	int cached_buffer_size = 0;
	std::atomic<bool> stop_settings_refresh{false};
	std::thread settings_refresh_thread;
	void refreshSettings();
};