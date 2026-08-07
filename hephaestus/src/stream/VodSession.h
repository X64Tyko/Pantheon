#pragma once
#include "ChannelSession.h" // HwAccel
#include "ClientCapabilities.h"
#include "FfmpegProcess.h"
#include "MediaProbe.h"
#include "VodEncodeStream.h"
#include "model/ExternalSubtitle.h" // shared/ — see that header for why
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

class VodSessionManager; // VodSessionManager.h includes this header — forward-declared to avoid the cycle

struct VodStreamOptions
{
	std::string ffprobe_path = "ffprobe";
	// VOD sessions live at "<hls_root>/vod/<session_id>/". Never empty in
	// practice — VOD has no non-HLS fallback the way live channels do.
	std::string hls_root;
	int linger_secs  = 60;
	int buffer_size  = 1048576;
	HwAccel hw_accel = HwAccel::none; // resolved encode backend (HwProbe)
	// Resolved decode backend + which source video codecs it can hwaccel-
	// decode, from HwProbe::probeHwCapabilities() at startup. Independent of
	// hw_accel above -- see EncoderArgs.h's pushHwAccelDecodeArgs.
	HwAccel decode_hw_accel = HwAccel::none;
	std::set<std::string> decodable_codecs;
	std::string vaapi_device    = "/dev/dri/renderD128";
	bool ffmpeg_debug_logs      = false;
	bool verbose_transcode_logs = false; // -v verbose + full command line on every spawn
	// Host-wide hardware-encode-session cap shared with channels/preview —
	// see EncoderAdmission's own class comment. nullptr (default) disables
	// gating entirely, same as every other caller of it.
	EncoderAdmission* encoder_admission = nullptr;
	// In-memory segment cache shared with channels/preview — nullptr
	// (default) disables caching entirely.
	SegmentCache* segment_cache = nullptr;
	// How far ahead of the viewer's last-requested segment the main encoder
	// is allowed to run before being paused (SIGSTOP) — see the lookahead
	// monitor (lookaheadLoop()). A few minutes, mirroring how
	// linger_secs/kVodHlsSegmentSecs are already simple, operator-tunable
	// scalars rather than hardcoded deep in the implementation.
	int lookahead_secs = 180;
};

// One viewer's own handle onto playback — but no longer "one file, one
// viewer" underneath. video_stream_/audio_stream_ are shared_ptrs onto
// VodEncodeStream instances that VodSessionManager hands out keyed by
// content + the resolved transcode decision (VideoStreamKey/AudioStreamKey,
// VodSessionManager.h) — a second viewer whose key matches an already-
// running stream attaches to it via ordinary shared_ptr refcounting, no
// "join" protocol needed. This facade still owns its own session_id and
// static playlist (every viewer gets their own manifest_url, unchanged
// protocol-wise), and still owns the *decisions* (probing, direct-stream,
// track resolution) that produce the keys — VodSessionManager just owns the
// sharing registry, not those decisions.
//
// The full HLS playlist (every segment's URI + EXTINF, #EXT-X-ENDLIST) is
// synthesized by THIS class from an authoritative duration and served
// immediately, in full, from the very first request — not derived from
// ffmpeg's own incremental "event" playlist output the way it used to be.
// This is what fixes VOD's wrong/unresolved player duration: hls.js/ExoPlayer
// see the real total duration on the very first manifest fetch.
//
// Each VodEncodeStream paces its own bounded-window "heads" independently
// (pause/resume, spawn-on-seek) — see VodEncodeStream.h. A TRACK switch
// (ensureAudioTrack) attaches audio_stream_ to a different shared stream
// (or creates one); it never restarts video_stream_, which a plain audio
// track selection has no bearing on at all now.
//
// Subtitle extraction is a second, fully independent FfmpegProcess covering
// the whole file in one small, fast pass, spawned once at start() and never
// shared across sessions (cheap enough not to bother) — see subs_ffmpeg.
class VodSession
{
public:
	VodSession(std::string session_id, std::string content_type, std::string content_id,
			   std::string ffmpeg_path, VodStreamOptions opts, VodSessionManager& manager);
	~VodSession();

	VodSession(const VodSession&)            = delete;
	VodSession& operator=(const VodSession&) = delete;

	// Probes file_path, decides direct-stream vs transcode, and spawns the main
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
	// isDirectStreamable() falls back to the conservative h264/aac allowlist.
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
	// kairos_keyframes_ms/_size/_mtime are Kairos's own cached direct-stream
	// keyframe probe for this file (from its sync-time scan — see
	// Database.cpp's v98 migration), used by computeSegmentBoundaries() in
	// place of Hephaestus's own ffprobe scan when kairos_keyframes_size/
	// _mtime still match the file's current stat(). Empty/0 (the default)
	// just means "nothing cached yet" — falls back to a live probe exactly
	// as before.
	bool start(const std::string& file_path, int64_t position_ms,
			   int audio_track, int subtitle_track, bool hdr_capable,
			   const std::optional<ClientCapabilities>& client_caps,
			   const std::vector<ExternalSubtitle>& external_subtitles,
			   int64_t fallback_duration_ms,
			   const std::string& preferred_audio_lang         = "",
			   const std::string& preferred_subtitle_lang      = "",
			   const std::vector<int64_t>& kairos_keyframes_ms = {},
			   int64_t kairos_keyframes_size                   = 0,
			   int64_t kairos_keyframes_mtime                  = 0);

	void stop();
	// Called by the HTTP handler on every playlist/segment/subs.vtt GET.
	void touch();

	// Router-facing decision for a requested segment index — call once per
	// seg-NNNNN.ts GET. Ready: already on disk, serve immediately (also
	// opportunistically resumes a paused head if the viewer's caught back up
	// close enough). WaitShort: within the lookahead margin just past what's
	// generated so far — resume if paused, then wait briefly. WaitColdStart:
	// this needed a fresh head (a real seek beyond any live head's reach) —
	// wait for it to spin up. Failed: index out of range or session no
	// longer active. Delegates to video_stream_'s own VodEncodeStream::
	// prepareSegment() — see prepareAudioSegment() for the audio-route twin.
	enum class SegmentPrep { Ready, WaitShort, WaitColdStart, Failed };

	SegmentPrep prepareSegment(int segment_index);
	// Same contract as prepareSegment(), against audio_stream_ — see
	// Router.cpp's /audio/{n}/seg-{n}.ts route. Failed if this file has no
	// audio at all (audio_stream_ is null).
	SegmentPrep prepareAudioSegment(int segment_index);

	bool isActive() const { return active.load(); }
	bool isIdle() const;
	const std::string& sessionId() const { return session_id; }
	std::string dir() const { return opts.hls_root + "/vod/" + session_id; }
	bool directStream() const { return direct_stream; }
	const MediaInfo& tracks() const { return media_info; }
	bool hasSubtitleOutput() const { return subtitle_output; }
	// The actually-resolved selection — may differ from what the client
	// requested (a saved preference, or -1/"unset" resolved to a real
	// default; see start()'s pickAudioTrack/pickSubtitleTrack calls).
	// Router.cpp echoes these back in /stream/vod/start's response so the
	// client's own track-selection state starts in sync with what's really
	// playing, since with the master manifest driving playback directly
	// (buildMasterPlaylist) nothing else tells it which rendition is
	// already DEFAULT="YES".
	int audioTrack() const { return audio_track_; }
	int subtitleTrack() const { return subtitle_track_; }
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
	int highestGeneratedSegment() const { return video_stream_ ? video_stream_->highestGeneratedSegment() : -1; }
	int lastRequestedSegment() const { return last_requested_segment.load(); }
	bool isMainEncoderPaused() const { return video_stream_ && video_stream_->anyHeadPaused(); }

	// On-disk path for a given segment, resolved through the (possibly
	// shared) underlying stream rather than this session's own dir() — see
	// VodEncodeStream::segmentPath()'s comment on why segment storage is
	// keyed by stream, not by viewer. Router.cpp's playlist/segment routes
	// use these instead of reconstructing paths from session->dir(). Empty
	// string if the respective stream doesn't exist (e.g. no audio track).
	std::string videoSegmentFilePath(int index) const { return video_stream_ ? video_stream_->segmentPath(index) : std::string(); }
	std::string audioSegmentFilePath(int index) const { return audio_stream_ ? audio_stream_->segmentPath(index) : std::string(); }

	// Synthesizes the multi-rendition HLS master manifest (#EXT-X-MEDIA
	// AUDIO/SUBTITLES groups + a single #EXT-X-STREAM-INF variant pointing at
	// the existing playlist.m3u8) — lets native players (ExoPlayer, TV
	// browsers, hls.js) offer their own audio/subtitle-language picker
	// instead of Hades' custom one. See ensureAudioTrack()/resolveSubtitleTrack()
	// for how the per-track routes this references actually get served.
	std::string buildMasterPlaylist() const;

	// RFC 6381 CODECS string for buildMasterPlaylist's #EXT-X-STREAM-INF —
	// see VodSession.cpp's own comment on why this only ever applies to a
	// direct-stream session and can return nullopt (omit CODECS) rather than
	// guess. Public so Router.cpp's diagnostic/debug routes can inspect it
	// directly if needed, same visibility as buildMasterPlaylist itself.
	std::optional<std::string> buildCodecsAttribute() const;

	// A minimal single-segment VOD media playlist wrapping the on-demand
	// WebVTT pipe (see spawnSubtitlePipe) — required because an
	// #EXT-X-MEDIA URI must reference a Media Playlist per the HLS spec, not
	// a raw resource directly. Pointing SUBTITLES groups straight at the
	// pipe endpoint made hls.js fail to parse it as a playlist and retry in
	// a tight loop (observed spawning the same extraction repeatedly in
	// rapid succession); wrapping it fetches the actual content exactly
	// once, same as a normal VOD text track. nullopt if track doesn't
	// resolve to anything extractable (same check spawnSubtitlePipe uses).
	std::optional<std::string> buildSubtitlePlaylist(int track) const;

	// Called by the /audio/{n}/... routes before serving the playlist/segment
	// under them. No-op if `track` is already active. Otherwise re-resolves
	// direct-stream eligibility for the new track (different audio tracks can
	// differ in codec — e.g. a commentary track in AC3 vs. a default AAC
	// track — so this can flip) and swaps audio_stream_ for whichever shared
	// stream the new track's AudioStreamKey resolves to (rebuildAudioStream)
	// — attaching to an already-running one if some other viewer already
	// wants that exact track/codec, or spawning a fresh one otherwise. Never
	// touches video_stream_ — audio and video are fully independent streams
	// now, see the class comment.
	bool ensureAudioTrack(int track);

	// Resolution of an arbitrary subtitle track index (embedded relative_index
	// >= 0, or the external-sidecar -2/-3/... scheme — see Router.cpp's
	// /stream/vod/start handler, the sole owner of that convention) into
	// what buildVodSubtitleArgs needs. Shared by start() (for the initially
	// selected track) and the /subtitles/{n} route (for any track a native
	// player asks for via the master manifest).
	struct SubtitleResolution
	{
		bool output  = false;      // extractable as a text sidecar
		bool burn_in = false;      // bitmap track — no sidecar, baked into video instead
		std::string external_path; // empty if embedded
	};

	SubtitleResolution resolveSubtitleTrack(int track) const;

	// Spawns a one-shot, on-demand WebVTT extraction for an arbitrary
	// subtitle track (any index resolveSubtitleTrack accepts), streaming
	// output through on_data instead of writing to disk — see
	// buildVodSubtitleArgs' on_fly parameter. Caller (Router's /subtitles/{n}
	// route) owns the returned process and pipes on_data straight into the
	// HTTP response; nullptr if the track doesn't resolve to anything
	// extractable. Independent of spawnSubtitleProcess()/subs_ffmpeg (the
	// start()-time extraction of the initially selected track) — this can
	// run in parallel with that or with itself for a different track, and
	// never touches audio_track_/the main encoder.
	std::unique_ptr<FfmpegProcess> spawnSubtitlePipe(int track, DataCallback on_data, ExitCallback on_exit) const;

	// Lazily spawns the whole-file /subs.vtt extraction (the initially-
	// selected track only — see the class comment) on first request instead
	// of unconditionally at start() — nothing built against this codebase
	// today (Hades, Android) still reads subtitle_url/GET /subs.vtt, both
	// having moved to spawnSubtitlePipe()'s per-track on-demand route, but
	// other clients (e.g. a Roku receiver, not in this repo) may still rely
	// on it, so the route stays rather than being removed outright — this
	// just stops paying for it when nothing asks. Idempotent: a no-op if
	// already spawned/finished, or if this session has no subtitle_output.
	void spawnSubtitleProcess();

	// For the activity/debugging view (ActivityRouter).
	const std::string& filePath() const { return file_path; }
	HwAccel hwAccel() const { return opts.hw_accel; }
	HwAccel decodeHwAccel() const { return opts.decode_hw_accel; }
	int64_t startedAtMs() const { return started_at_ms; }

private:
	std::string session_id;
	std::string content_type_;
	std::string content_id_;
	std::string ffmpeg_path;
	VodStreamOptions opts;
	// The registry this session's shared video_stream_/audio_stream_ come
	// from — a plain reference, not owned; VodSessionManager outlives every
	// VodSession it hands out (it owns them via `sessions`), so this is safe
	// for this facade's whole lifetime. Used by start()/rebuildAudioStream()
	// to call getOrCreateVideoStream()/getOrCreateAudioStream().
	VodSessionManager& manager_;

	// Guards session-level state that changes together on start()/track
	// switches: total_segments/segment_start_ms recomputation and swapping
	// audio_stream_ out for a different track. video_stream_/audio_stream_
	// each own their own internal locking for their own head bookkeeping —
	// this mutex is about VodSession's own state, not theirs.
	std::mutex session_mtx;

	// One shared VodEncodeStream per elementary stream — see
	// VodEncodeStream.h's class comment for why video and audio are no
	// longer a single muxed process, and VodSessionManager.h for why these
	// are shared_ptrs onto content-keyed streams rather than this session's
	// own private instances. video_stream_ is fixed for the session's life
	// (a video-affecting change, e.g. burn-in track, needs a fresh
	// VodSession/session_id, not a swap here); audio_stream_ gets replaced
	// wholesale by ensureAudioTrack() when the selected track changes (a
	// different key resolves to a different — or newly created — shared
	// stream; this facade just stops holding a reference to the old one).
	std::shared_ptr<VodEncodeStream> video_stream_;
	std::shared_ptr<VodEncodeStream> audio_stream_; // null if the file has no audio at all

	// Debug/activity-view aggregate — see prepareSegment()/prepareAudioSegment().
	// Not used for any real pacing/restart decision anymore (that's each
	// VodEncodeStream's own per-head bookkeeping); purely what ActivityRouter
	// polls for "what segment is being asked for right now."
	std::atomic<int> last_requested_segment{-1};

	int64_t effective_duration_ms = 0;
	int total_segments            = 0;
	// Segment i spans [segment_start_ms[i], segment_start_ms[i+1] or
	// effective_duration_ms for the last one). Computed in start() and, on a
	// track switch that flips VIDEO direct-stream eligibility (burn-in can
	// force a video re-encode), recomputed by ensureAudioTrack() via
	// computeSegmentBoundaries(). Shared by both video_stream_ and
	// audio_stream_ — they must always agree on where segment N starts, even
	// though each produces its own segment files for it.
	std::vector<int64_t> segment_start_ms;
	// Shared by start() and ensureAudioTrack() — (re)computes segment_start_ms/
	// total_segments from the current direct_stream/file_path/effective_duration_ms.
	void computeSegmentBoundaries();

	// Independent subtitle extraction — see class comment.
	std::mutex subs_mtx;
	std::unique_ptr<FfmpegProcess> subs_ffmpeg;
	std::atomic<bool> subs_exited{false};
	std::atomic<bool> subs_failed{false};

	// Lookahead monitor — see lookaheadLoop(). Now just ticks both streams;
	// all the actual scan/pause/reap logic lives in VodEncodeStream::tick().
	std::thread lookahead_thread;
	std::atomic<bool> lookahead_stop{false};
	void lookaheadLoop();

	// Requires session_mtx already held by the caller.
	std::string buildStaticPlaylist(const std::string& segment_prefix) const;
	void writeVideoPlaylist() const;
	void writeAudioPlaylist() const;

	std::atomic<bool> active{false};
	std::atomic<int64_t> last_touch_ms{0};
	// Video-only now — see isVideoDirectStreamable(). Audio's own direct-stream
	// decision is made fresh per track inside buildVodAudioArgs(), not
	// stored here, since it no longer has any bearing on the video stream.
	bool direct_stream    = false;
	bool subtitle_output  = false;
	bool subtitle_burn_in = false;
	MediaInfo media_info;
	std::string file_path;
	int64_t started_at_ms = 0;
	// Kairos's cached direct-stream keyframe probe, and the file stat() it
	// was computed from — see start()'s own comment and
	// computeSegmentBoundaries(), the only reader.
	std::vector<int64_t> kairos_keyframes_ms_;
	int64_t kairos_keyframes_size_  = 0;
	int64_t kairos_keyframes_mtime_ = 0;
	std::vector<ExternalSubtitle> external_subtitles_;

	// Resolved once in start(), reused by every subsequent head spawn — a
	// seek keeps the same track/HDR selection, only the position changes.
	// audio_track_ is the one exception: ensureAudioTrack() (a track switch
	// via the master manifest's /audio/{n}/... routes) reassigns it.
	int audio_track_    = 0;
	int subtitle_track_ = -1;
	bool hdr_capable_   = false;
	std::string source_codec_;
	std::string external_subtitle_path_;
	// The requesting client's declared decode capability from start() —
	// stashed so ensureAudioTrack() can re-run isAudioDirectStreamable() for a
	// different audio track's codec without needing it passed in again.
	std::optional<ClientCapabilities> client_caps_;

	// Builds a fresh audio_stream_ targeting audio_track_ (whatever it's
	// currently set to) and kicks off its first head at target_segment.
	// Called from start() (the session's initial position) and
	// ensureAudioTrack() (wherever the viewer currently is) on an actual
	// track change.
	void rebuildAudioStream(int target_segment);
};