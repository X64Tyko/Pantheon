#pragma once
#include "../kairos/KairosClient.h"
#include "../kairos/KairosTypes.h"
#include "FfmpegProcess.h"
#include "MediaProbe.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <set>
#include <thread>

// One ClientSink per connected HTTP client. Thread-safe queue the HTTP handler
// thread reads from while the session's reader thread writes to it.
struct ClientSink
{
	std::mutex mtx;
	std::condition_variable cv;
	std::deque<std::vector<uint8_t>> queue;
	std::atomic<bool> done{false};
	static constexpr size_t MAX_QUEUE = 64; // ~8 MB at 128 KB chunks; slow clients are dropped
};

enum class HwAccel { none, nvidia, amd };

struct StreamOptions
{
	std::string ffprobe_path  = "ffprobe";
	std::string audio_lang    = "eng";
	std::string subtitle_lang = ""; // empty = no subtitle mapping
	bool loudnorm             = false;
	int linger_secs           = 60;
	int buffer_size           = 1048576;       // 1024 KB
	HwAccel hw_accel          = HwAccel::none; // resolved encode backend (HwProbe)
	// Resolved decode backend + which source video codecs it can hwaccel-
	// decode, from HwProbe::probeHwCapabilities() at startup. Independent of
	// hw_accel above -- see EncoderArgs.h's pushHwAccelDecodeArgs.
	HwAccel decode_hw_accel = HwAccel::none;
	std::set<std::string> decodable_codecs;
	std::string vaapi_device    = "/dev/dri/renderD128";
	bool ffmpeg_debug_logs      = false; // pipe ffmpeg stderr into the log stream
	bool verbose_transcode_logs = false; // -v verbose + full command line on every spawn
	// Per-channel transcode quality
	std::string max_resolution = "source"; // "source"|"1080p"|"720p"|"480p"
	int video_bitrate_kbps     = 0;        // 0 = CRF/CQ auto; >0 adds -maxrate cap
	int audio_bitrate_kbps     = 192;      // kbps for -b:a
	// Offline/splash image fallback
	std::string logo_path;         // per-channel logo, empty = none configured
	std::string default_logo_path; // bundled generic fallback, always set
	// HLS output for the web player. Empty = HLS disabled, legacy plain
	// MPEG-TS pipe:1 output only. When set, the live HLS directory is
	// "<hls_root>/live/<channel_id>/".
	std::string hls_root;
	// True disables the native/direct-stream bucket for this channel
	// entirely (ChannelViewerRegistry never resolves or spawns it) — mirrors
	// channel.force_transcode. ChannelSession itself only needs this to gate
	// speed-based drift correction (see applyResolvedItem()/transition()):
	// that's only safe when it's the *only* bucket in play for this channel.
	// A dual-bucket channel's default-bucket viewers must stay positionally
	// identical to its native-bucket ones — speeding up only the default
	// bucket would let two viewers of the "same" channel drift apart in
	// actual content position, which defeats the point of it being live.
	bool force_transcode = false;
};

// shared_ptr-owned by SessionManager, whose map entry can be overwritten with
// a fresh instance once this one goes inactive (see SessionManager.cpp) —
// enable_shared_from_this lets the background helper threads below
// (start()'s /now lookup, spawnOffline/transition re-dispatch, scheduleStop's
// linger) keep the session alive for as long as they run instead of touching
// a freed `this`.
class ChannelSession : public std::enable_shared_from_this<ChannelSession>
{
public:
	// Bucket identity — "default" is the legacy/universal transcode session
	// (byte-for-byte the original single-session-per-channel behavior);
	// "native" copies the source codec instead of re-encoding it, for
	// viewers whose declared capabilities make that possible for whatever's
	// currently airing. See ChannelViewerRegistry for the resolution logic
	// and the plan's "two buckets" scope decision — v1 has only these two.
	static constexpr const char* kDefaultBucket = "default";
	static constexpr const char* kNativeBucket  = "native";

private:
	std::string channel_id; // Kairos channel UUID
	KairosClient& kairos;
	std::string ffmpeg_path;
	StreamOptions opts;
	std::string bucket = kDefaultBucket; // see kDefaultBucket/kNativeBucket above

	std::mutex clients_mtx;
	std::vector<std::shared_ptr<ClientSink>> clients;
	std::atomic<int> client_count{0};
	std::atomic<int> stop_token{0}; // incremented on each scheduleStop call

	std::mutex ffmpeg_mtx;
	std::unique_ptr<FfmpegProcess> ffmpeg;

	// current_item is a KairosNowResponse (several std::strings + a vector),
	// whole-struct-assigned on the scheduling thread (applyResolvedItem()/
	// transition()) while also read from other threads (prefetchLoop(),
	// currentTitle()/currentFilePath() below) — an unsynchronized concurrent
	// read during that assignment is a real data race (UB on the std::string
	// fields, not just a stale-value risk), so every access goes through
	// current_item_mtx: writers hold it only around the assignment itself
	// (never across kairos.* network calls), readers take a copy under it.
	mutable std::mutex current_item_mtx;
	KairosNowResponse current_item;
	std::chrono::steady_clock::time_point item_start;
	int64_t current_item_offset_ms = 0; // offset into current_item's own file we started at
	int64_t session_start_ms       = 0; // wall-clock start() time, for the activity view

	std::atomic<bool> active{false};
	std::atomic<bool> in_splash{false}; // true while showing the connect-time logo splash

	// HLS liveness tracking. HLS has no persistent connection to signal
	// "viewer disconnected" the way the MPEG-TS ClientSink model does (an
	// HLS player just polls the playlist) — a background watcher thread and
	// a last-touch timestamp stand in for that.
	std::atomic<int64_t> last_hls_touch_ms{0};
	std::thread hls_watcher;
	std::atomic<bool> hls_watcher_stop{false};
	void hlsWatchLoop();
	bool hlsIdle() const;

	// ffmpeg's HLS muxer (append_list+delete_segments) inserts
	// #EXT-X-DISCONTINUITY at each new spawnFfmpeg() (a new process
	// restart), but never maintains #EXT-X-DISCONTINUITY-SEQUENCE once an
	// earlier one rolls off the sliding window — an HLS-spec requirement
	// (RFC 8216 §4.3.3) Roku's player enforces strictly and stops playback
	// over. No ffmpeg hls_flags option controls this (checked against
	// ffmpeg n8.1.1's own muxer help), so a small background loop patches
	// the on-disk playlist.m3u8 directly. One count per spawnFfmpeg() after
	// the very first (transition() increments it); the first spawn isn't a
	// discontinuity.
	std::atomic<int> discontinuity_count_{0};
	std::thread hls_patch_thread;
	std::atomic<bool> hls_patch_stop{false};
	void hlsPatchLoop();
	void patchDiscontinuitySequence();
	// Last segment URI whose EXTINF duration was already checked/logged by
	// patchDiscontinuitySequence's verbose_transcode_logs diagnostic — not
	// synchronized, only ever touched from the single hlsPatchLoop thread.
	std::string last_extinf_uri_;

	// Warms Hephaestus's own file probe cache (MediaProbe.cpp's process-
	// lifetime probeMediaCached cache) for the *next* scheduled item a few
	// seconds before transition() actually needs it, instead of paying for
	// that ffprobe cold on the transition hot path — see prefetchLoop()'s own
	// comment. Runs for the life of the session regardless of HLS/MPEG-TS
	// output mode (unlike hls_watcher/hls_patch_thread above, which are
	// HLS-only), so it's spawned/joined independently of hlsDir().
	std::thread prefetch_thread;
	std::atomic<bool> prefetch_stop{false};
	// wall_clock_end_ms of the current item we've already issued a prefetch
	// for — guards against re-issuing the same /next lookup on every tick
	// while still inside the lead window.
	std::atomic<int64_t> prefetched_for_end_ms{0};
	void prefetchLoop();

	// Last item's probed MediaInfo + resolved audio track, cached off the
	// scheduling thread so a fresh POST /stream/channel/:id/start can read
	// back this session's authoritative decision (real audio_lang-driven
	// track) instead of guessing — see currentMediaInfo()/currentAudioTrack().
	mutable std::mutex info_mtx;
	std::optional<MediaInfo> current_info;
	int current_audio_track = 0;

	void onData(const uint8_t* data, size_t len);
	void onExit(int code);
	void transition();
	// Applies a resolved /now lookup (or its absence, on failure): computes
	// start offset/speed and spawns ffmpeg for it, or falls back to the
	// splash. Shared by start()'s fast (answered before kFastPathBudget) and
	// slow (answered later, on its own thread) paths.
	void applyResolvedItem(std::optional<KairosNowResponse> item, int64_t at);
	void spawnFfmpeg(const KairosNowResponse& item, int64_t startOffsetMs, double speed = 1.0);
	void spawnOffline(const KairosNowResponse& item);
	void launchFfmpeg(std::vector<std::string> args, const char* what);
	void broadcastDone();
	void scheduleStop();

	// Computes how far into `item` playback should start, given the true
	// wall-clock time `atMs`. Loops fillers on their own duration; clamps to 0
	// for non-fillers that haven't started yet.
	static int64_t computeOffset(const KairosNowResponse& item, int64_t atMs);

	// Given raw (unclamped, signed) drift = actualNowMs - item.wall_clock_start_ms
	// and the item's known duration, decides whether the drift can be closed
	// gently over the course of playing the *entire* item at a slightly
	// adjusted speed (positive drift == running behind schedule == speed up;
	// negative == running ahead == slow down) rather than seeking into/
	// skipping content. Returns nullopt when the item has no known duration,
	// or the drift is too large to close within a small, near-imperceptible
	// speed adjustment — callers should fall back to offset-based seeking.
	static std::optional<double> computeSpeed(int64_t rawDriftMs, int64_t durationMs);

	// Snaps offsetMs down to the nearest real keyframe at/before it, using
	// item's cached keyframes_ms (Kairos's own sync-time probe — see
	// KairosNowResponse's own comment) if it's still valid for the file's
	// current size/mtime. A direct-stream (stream copy) session can only ever
	// start output at a real keyframe regardless — this makes that landing
	// point a precomputed lookup instead of ffmpeg's own blind seek-and-
	// search on every transition. Returns offsetMs unchanged (today's
	// behavior) when the cache is empty or stale — deliberately no fallback
	// probe here: an unsynced file just rides on the old behavior until the
	// next Kairos sync populates the cache, rather than reintroducing the
	// lazy-probe cost this exists to remove.
	static int64_t snapToKeyframe(const KairosNowResponse& item, int64_t offsetMs);

public:
	// Exposed for testing
	static int64_t computeOffsetForTest(const KairosNowResponse& item, int64_t atMs)
	{
		return computeOffset(item, atMs);
	}

	static std::optional<double> computeSpeedForTest(int64_t rawDriftMs, int64_t durationMs)
	{
		return computeSpeed(rawDriftMs, durationMs);
	}

	void setDiscontinuityCountForTest(int n) { discontinuity_count_.store(n); }
	void patchDiscontinuitySequenceForTest() { patchDiscontinuitySequence(); }

	// Exposed for testing the start()/applyResolvedItem() retry-on-
	// unreachable-Kairos fix: whether the session is still showing the
	// unbounded cold-start splash (should be false once the bounded-retry
	// fallback has taken over) and the current_item snapshot it should have
	// switched to instead (a synthetic "offline" item with a real, bounded
	// wall_clock_end_ms) — see applyResolvedItem()'s own comment.
	bool inSplashForTest() const { return in_splash.load(); }

	std::string currentItemTypeForTest() const
	{
		std::lock_guard<std::mutex> l(current_item_mtx);
		return current_item.item_type;
	}

	int64_t currentItemWallClockEndMsForTest() const
	{
		std::lock_guard<std::mutex> l(current_item_mtx);
		return current_item.wall_clock_end_ms;
	}

	ChannelSession(std::string channel_id, KairosClient& kairos,
				   std::string ffmpeg_path, StreamOptions opts = {},
				   std::string bucket                          = kDefaultBucket);
	~ChannelSession();

	// Notified from spawnFfmpeg() whenever a new (non-offline) item starts
	// playing, so a ChannelViewerRegistry can keep capability-bucketed
	// viewers' *recommended* bucket current as the schedule advances (see
	// ChannelViewerRegistry::reassignForChannel — it no longer silently
	// migrates an already-connected viewer's serving bucket; a real bucket
	// change only ever happens via a fresh reconnect). item_duration_ms/
	// is_filler let that recommendation ignore short bumpers/fillers, where a
	// reconnect's own rebuffer would cost more than the transcode CPU it
	// would save. Public field, not a setter — set once by SessionManager
	// right after construction. Unset (nullptr) is harmless: live-channel
	// bucketing simply never engages, everything behaves exactly as it did
	// before this feature.
	std::function<void(const std::string & channel_id, const std::optional<MediaInfo> & info, int audio_track,
					   int64_t item_duration_ms, bool is_filler)> onItemPlaying;

	// Fetches current item from Kairos and starts ffmpeg. Returns false on failure.
	bool start();

	void stop();

	void addClient(std::shared_ptr<ClientSink> sink);
	void removeClient(std::shared_ptr<ClientSink> sink);

	// Best-effort snapshot of the last item this session resolved — see
	// info_mtx/current_info's own comment.
	std::optional<MediaInfo> currentMediaInfo() const
	{
		std::lock_guard<std::mutex> l(info_mtx);
		return current_info;
	}

	int currentAudioTrack() const
	{
		std::lock_guard<std::mutex> l(info_mtx);
		return current_audio_track;
	}

	// Directory ffmpeg writes the live HLS playlist/segments to. Empty when
	// HLS is disabled (opts.hls_root empty).
	std::string hlsDir() const;
	// Called by the HTTP handler on every HLS playlist/segment GET — keeps
	// the session alive the same way an MPEG-TS client connection does.
	void touchHls();

	bool isActive() const { return active.load(); }
	const std::string& channelId() const { return channel_id; }
	const std::string& bucketName() const { return bucket; }

	// Best-effort snapshot for the activity/debugging view (ActivityRouter) —
	// by value under current_item_mtx (see its own comment): the value can
	// still be a tick stale by the time the caller uses it, same tradeoff as
	// before, but the read itself is now race-free.
	std::string currentTitle() const
	{
		std::lock_guard<std::mutex> l(current_item_mtx);
		return current_item.title;
	}

	std::string currentFilePath() const
	{
		std::lock_guard<std::mutex> l(current_item_mtx);
		return current_item.file_path;
	}

	HwAccel hwAccel() const { return opts.hw_accel; }
	HwAccel decodeHwAccel() const { return opts.decode_hw_accel; }
	int64_t sessionStartMs() const { return session_start_ms; }

	// Connected native MPEG-TS/DVR clients — an exact count (see addClient/
	// removeClient). Does NOT include HLS viewers: HLS has no persistent
	// connection to count the way a ClientSink does, see hlsViewerActive.
	int clientCount() const { return client_count.load(); }

	// Best-effort "someone is actively watching over HLS right now" signal —
	// just whether the playlist/segments have been requested within the
	// linger window (touchHls), not a count. True doesn't distinguish one
	// HLS viewer from several; there's currently no per-viewer identity in
	// an HLS request to count against (see touchHls's own comment).
	bool hlsViewerActive() const { return !hlsIdle(); }
};