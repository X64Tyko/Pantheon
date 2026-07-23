#pragma once
#include "VodSession.h"
#include "../kairos/KairosClient.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Keyed by generated session_id, not channel_id — each VOD viewer gets an
// independent session (unlike SessionManager's per-channel shared sessions).
// A TRACK SWITCH is still "stop the old session, create a new one" (a
// different audio/subtitle selection needs genuinely different ffmpeg -map
// args) — but a plain SEEK is no longer handled here at all: VodSession now
// stays alive across seeks, restarting its own internal encoder in place
// (see VodSession::restartAt) while keeping the same session_id/manifest for
// its whole life. So this class is still simple, just not "no reuse" in the
// seek sense anymore — only track switches ever call create() a second time
// for what a viewer experiences as one continuous playback.
class VodSessionManager {
public:
	VodSessionManager(std::string ffmpeg_path, VodStreamOptions opts, KairosClient& kairos);
	~VodSessionManager();

	// Creates and starts a new session. Returns nullptr if start() fails
	// (probe failure, no duration available, or ffmpeg wouldn't spawn).
	// hdr_capable is the requesting client's own display capability,
	// client_caps its declared decode capability if any (see
	// VodSession::start). fallback_duration_ms is Kairos's own library-scan
	// duration, used only if this file's own ffprobe run doesn't report one.
	// preferred_audio_lang/preferred_subtitle_lang are the caller's saved
	// per-show preference (Kairos), used only when audio_track/subtitle_track
	// are left unset (-1) by the client itself.
	std::shared_ptr<VodSession> create(const std::string& file_path, int64_t position_ms,
										int audio_track, int subtitle_track, bool hdr_capable,
										const std::optional<ClientCapabilities>& client_caps,
										const std::vector<ExternalSubtitle>& external_subtitles,
										int64_t fallback_duration_ms,
										const std::string& preferred_audio_lang = "",
										const std::string& preferred_subtitle_lang = "");
	std::shared_ptr<VodSession> get(const std::string& sessionId);
	void stop(const std::string& sessionId);

	// Snapshot of currently-active sessions, for the activity/debugging view
	// (ActivityRouter).
	std::vector<std::shared_ptr<VodSession>> listActive();

private:
	std::string      ffmpeg_path;
	VodStreamOptions opts;
	KairosClient&    kairos;

	std::mutex mtx;
	std::map<std::string, std::shared_ptr<VodSession>> sessions;

	// HLS is poll-based (no persistent connection to react to), so idle
	// sessions are swept periodically rather than torn down on disconnect —
	// same reasoning as ChannelSession's hlsWatchLoop.
	std::atomic<bool> stop_reaper{false};
	std::thread       reaper_thread;
	void reapLoop();

	// Mirrors SessionManager's refreshCache()/kCacheRefreshInterval: applies
	// Kairos-driven settings live (currently verbose_transcode_logs and
	// buffer_size, the two global — not per-channel — settings that also
	// apply to VOD) without needing a Hephaestus restart. VOD has no
	// per-channel config the way live channels do, so there's no channel
	// list to cache here, just these two scalars.
	std::mutex          settings_mtx;
	std::optional<bool> cached_verbose_transcode_logs;
	int                 cached_buffer_size = 0;
	std::atomic<bool>   stop_settings_refresh{false};
	std::thread         settings_refresh_thread;
	void refreshSettings();
};
