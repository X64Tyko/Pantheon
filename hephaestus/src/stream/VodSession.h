#pragma once
#include "ChannelSession.h" // HwAccel
#include "ClientCapabilities.h"
#include "FfmpegProcess.h"
#include "MediaProbe.h"
#include "model/ExternalSubtitle.h" // shared/ — see that header for why
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct VodStreamOptions {
	std::string ffprobe_path = "ffprobe";
	// VOD sessions live at "<hls_root>/vod/<session_id>/". Never empty in
	// practice — VOD has no non-HLS fallback the way live channels do.
	std::string hls_root;
	int         linger_secs       = 60;
	int         buffer_size       = 1048576;
	HwAccel     hw_accel          = HwAccel::none; // resolved encode backend (HwProbe)
	// Resolved decode backend + which source video codecs it can hwaccel-
	// decode, from HwProbe::probeHwCapabilities() at startup. Independent of
	// hw_accel above -- see EncoderArgs.h's pushHwAccelDecodeArgs.
	HwAccel     decode_hw_accel   = HwAccel::none;
	std::set<std::string> decodable_codecs;
	std::string vaapi_device      = "/dev/dri/renderD128";
	bool        ffmpeg_debug_logs = false;
	bool        verbose_transcode_logs = false; // -v verbose + full command line on every spawn
	// How far ahead of the viewer's last-requested segment the main encoder
	// is allowed to run before being paused (SIGSTOP) — see the lookahead
	// monitor (lookaheadLoop()). A few minutes, mirroring how
	// linger_secs/kVodHlsSegmentSecs are already simple, operator-tunable
	// scalars rather than hardcoded deep in the implementation.
	int         lookahead_secs    = 180;
};

// One file, one viewer. ffmpeg writes HLS segments straight to disk and the
// HTTP layer serves them as static files — same as before. What's changed
// from the original one-shot-encode design:
//
// The full HLS playlist (every segment's URI + EXTINF, #EXT-X-ENDLIST) is
// synthesized by THIS class from an authoritative duration and served
// immediately, in full, from the very first request — not derived from
// ffmpeg's own incremental "event" playlist output the way it used to be.
// This is what fixes VOD's wrong/unresolved player duration: hls.js/ExoPlayer
// see the real total duration on the very first manifest fetch.
//
// The main encoder still only runs one ffmpeg process at a time, but it's no
// longer a single one-shot invocation covering the whole remaining file: it
// gets paused (SIGSTOP) once it's generated `lookahead_secs` worth of
// segments ahead of what the viewer has actually requested, and resumed
// (SIGCONT) as playback catches up. A seek beyond the already-generated
// range kills and respawns it at the new position (see restartAt()) — same
// session_id/manifest the whole time, no more "seek = tear down this session
// and start a whole new one" (that's still how a TRACK switch works, via
// VodSessionManager — a different audio/subtitle selection needs genuinely
// different ffmpeg -map args, not just a different position).
//
// Subtitle extraction is a second, fully independent FfmpegProcess covering
// the whole file in one small, fast pass, spawned once at start() and never
// touched by the main encoder's pause/restart cycle — see subs_ffmpeg.
class VodSession {
public:
	VodSession(std::string session_id, std::string ffmpeg_path, VodStreamOptions opts);
	~VodSession();

	VodSession(const VodSession&)            = delete;
	VodSession& operator=(const VodSession&) = delete;

	// Probes file_path, decides direct-play vs transcode, and spawns the main
	// encoder + the independent subtitle extraction process. Returns false if
	// probing fails (file missing/unreadable), no duration is available from
	// either this probe or fallback_duration_ms, or ffmpeg won't start.
	// hdr_capable comes from the requesting client's own display capability
	// check (see api/client.ts's isHdrCapableDisplay) — an HDR source gets a
	// real HEVC Main10 HDR10 re-encode when true, or a real tone-map to
	// correct SDR when false (see EncoderArgs.cpp's pushVideoEncoderArgs).
	// client_caps is this specific client's declared decode capability (see
	// ClientCapabilities.h) — nullopt when the requesting token never
	// declared one (or Hephaestus restarted since), in which case
	// isDirectPlayable() falls back to the conservative h264/aac allowlist.
	// external_subtitles is the full catalog for this item (from Kairos's
	// playback-info response) — subtitle_track <= -2 selects
	// external_subtitles[-(subtitle_track)-2] (see Router.cpp's
	// /stream/vod/start handler for where that negative-index scheme is
	// assigned; both sides must stay in sync since nothing else enforces
	// it). >= 0 still means an embedded track's relative_index; -1 means no
	// subtitle at all. fallback_duration_ms is Kairos's own library-scan
	// duration, used only when this file's own ffprobe -show_format run
	// (MediaProbe's duration_ms) comes back 0 (raw/unusual containers).
	// preferred_audio_lang/preferred_subtitle_lang are the caller's saved
	// per-show preference (empty = none) — only consulted when
	// audio_track/subtitle_track are left at their "unset" default (-1) by
	// the client itself, which always wins if explicitly given.
	bool start(const std::string& file_path, int64_t position_ms,
			   int audio_track, int subtitle_track, bool hdr_capable,
			   const std::optional<ClientCapabilities>& client_caps,
			   const std::vector<ExternalSubtitle>& external_subtitles,
			   int64_t fallback_duration_ms,
			   const std::string& preferred_audio_lang = "",
			   const std::string& preferred_subtitle_lang = "");

	void stop();
	// Called by the HTTP handler on every playlist/segment/subs.vtt GET.
	void touch();

	// Router-facing decision for a requested segment index — call once per
	// seg-NNNNN.ts GET. Ready: already on disk, serve immediately (also
	// opportunistically resumes a paused encoder if the viewer's caught back
	// up close enough). WaitShort: within the lookahead margin just past
	// what's generated so far — resume if paused, then wait briefly.
	// WaitColdStart: this triggered a restartAt() (a real seek beyond reach,
	// forward or into a backward hole) — wait for a fresh encoder spin-up.
	// Failed: index out of range or session no longer active.
	enum class SegmentPrep { Ready, WaitShort, WaitColdStart, Failed };
	SegmentPrep prepareSegment(int segment_index);

	bool isActive() const { return active.load(); }
	bool isIdle() const;
	const std::string& sessionId() const { return session_id; }
	std::string dir() const { return opts.hls_root + "/vod/" + session_id; }
	bool directPlay() const { return direct_play; }
	const MediaInfo& tracks() const { return media_info; }
	bool hasSubtitleOutput() const { return subtitle_output; }
	// True when the selected subtitle track is a bitmap format (PGS/DVD/DVB)
	// being composited directly onto the video via ffmpeg's overlay filter,
	// rather than extracted as a WebVTT sidecar (hasSubtitleOutput()/
	// subtitle_url) — no separate subtitle URL exists for this case, it's
	// baked into the HLS video segments themselves.
	bool subtitleBurnedIn() const { return subtitle_burn_in; }
	// The full external-subtitle catalog this session was started with (not
	// just whichever one is selected) — Router.cpp lists all of them in
	// tracks.subtitles[] alongside the embedded ones.
	const std::vector<ExternalSubtitle>& externalSubtitles() const { return external_subtitles_; }
	// The independent subtitle-extraction process (see class comment) covers
	// the whole file in one small, fast pass — this is true once THAT
	// process (not the main encoder) has finished, which is what Router.cpp's
	// /subs.vtt handler waits on before serving (a <track>/ExoPlayer
	// sideloaded-subtitle fetch happens exactly once and never re-fetches, so
	// serving before the file is complete means permanently missing cues).
	bool hasSubtitleExtractionExited() const { return subs_exited.load(); }
	// True if the process exited non-zero (e.g. a charset ffmpeg couldn't
	// decode) — Router.cpp's /subs.vtt handler 503s on this instead of
	// serving the empty-but-valid file ffmpeg leaves behind on failure.
	bool hasSubtitleExtractionFailed() const { return subs_failed.load(); }

	// Authoritative duration (this session's own ffprobe run, or the
	// fallback passed to start() if that came back empty).
	int64_t durationMs() const { return effective_duration_ms; }
	int totalSegments() const { return total_segments; }
	int highestGeneratedSegment() const { return highest_generated_segment.load(); }
	int lastRequestedSegment() const { return last_requested_segment.load(); }
	bool isMainEncoderPaused() const;

	// For the activity/debugging view (ActivityRouter).
	const std::string& filePath() const { return file_path; }
	HwAccel hwAccel() const       { return opts.hw_accel; }
	HwAccel decodeHwAccel() const { return opts.decode_hw_accel; }
	int64_t startedAtMs() const   { return started_at_ms; }

private:
	std::string   session_id;
	std::string   ffmpeg_path;
	VodStreamOptions opts;

	// Guards: the main `ffmpeg` unique_ptr swap/reset, current_run_start_segment
	// and highest_generated_segment's RESET (its forward advancement by the
	// lookahead thread is lock-free atomic — see prepareSegment()'s own
	// comment on why that's safe), discontinuity_boundaries, and playlist
	// (re)writes — i.e. everything that changes atomically together whenever
	// the main encoder (re)starts at a new position.
	std::mutex    session_mtx;
	std::unique_ptr<FfmpegProcess> ffmpeg;
	std::atomic<int> current_run_start_segment{0};
	std::atomic<int> highest_generated_segment{-1};
	std::atomic<int> last_requested_segment{-1};
	std::vector<int> discontinuity_boundaries; // guarded by session_mtx
	bool has_spawned_once_ = false;            // guarded by session_mtx
	// The segment start() originally resolved from the caller's position_ms
	// — immutable, unlike current_run_start_segment. Distinguishes a real
	// backward seek from a stray early request before this session ever
	// reached its actual start — see prepareSegment().
	int initial_start_segment_ = 0;
	std::atomic<bool> initial_target_reached{false};
	// True once the CURRENT run's encoder has exited on its own (reached
	// real EOF or crashed) — reset every time restartAt() spawns a fresh
	// one. Lets prepareSegment() notice a run that finished without ever
	// producing every segment the static playlist declared for it (e.g. a
	// direct-play remux whose actual keyframe cadence is coarser than the
	// assumed uniform segment length — stream copy can't force keyframes)
	// and restart immediately instead of waiting out a budget on a process
	// that will never produce the missing segment.
	std::atomic<bool> current_run_exited_naturally{false};

	int64_t effective_duration_ms = 0;
	int     total_segments        = 0;
	// Segment i spans [segment_start_ms[i], segment_start_ms[i+1] or
	// effective_duration_ms for the last one). Computed once in start(),
	// immutable for the session's whole life (every restartAt() reuses it —
	// no re-probing per seek). For direct-play, these are the file's REAL
	// keyframe-derived cut points (see MediaProbe::probeKeyframeTimestampsMs
	// and simulateDirectPlaySegmentBoundaries in the .cpp) — stream copy
	// can't force keyframes, so segments can only ever be cut where they
	// already exist, and can run longer than kVodHlsSegmentSecs. For
	// transcode/burn-in, these are the synthetic uniform kVodHlsSegmentSecs
	// boundaries -force_key_frames actually produces.
	std::vector<int64_t> segment_start_ms;

	// Independent subtitle extraction — see class comment.
	std::mutex    subs_mtx;
	std::unique_ptr<FfmpegProcess> subs_ffmpeg;
	std::atomic<bool> subs_exited{false};
	std::atomic<bool> subs_failed{false};

	// Lookahead monitor — see lookaheadLoop().
	std::thread       lookahead_thread;
	std::atomic<bool> lookahead_stop{false};
	void lookaheadLoop();
	void scanGeneratedProgress();
	void maybeAutoPause();

	// Kills the current main ffmpeg (if any) and spawns a fresh one seeked to
	// segment_index, with -hls_start_number set so its output segment
	// numbering lines up with the already-declared static playlist. The
	// ONLY code path that ever spawns the main encoder — start() calls it
	// too — which is what guarantees segment numbering always matches.
	bool restartAt(int segment_index);
	void spawnSubtitleProcess();
	std::string segmentPath(int index) const;
	// Both require session_mtx already held by the caller (only ever called
	// from within restartAt()).
	std::string buildStaticPlaylist() const;
	void writePlaylist() const;

	std::atomic<bool>    active{false};
	std::atomic<int64_t> last_touch_ms{0};
	bool          direct_play     = false;
	bool          subtitle_output = false;
	bool          subtitle_burn_in = false;
	MediaInfo     media_info;
	std::string   file_path;
	int64_t       started_at_ms = 0;
	std::vector<ExternalSubtitle> external_subtitles_;

	// Resolved once in start(), reused by every restartAt() call — a seek
	// restart keeps the same track/HDR selection, only the position changes.
	int         audio_track_ = 0;
	int         subtitle_track_ = -1;
	bool        hdr_capable_ = false;
	std::string source_codec_;
	std::string external_subtitle_path_;

	void onMainEncoderExit(int code);
};
