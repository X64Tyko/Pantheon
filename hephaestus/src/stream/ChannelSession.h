#pragma once
#include "../kairos/KairosClient.h"
#include "../kairos/KairosTypes.h"
#include "EncoderAdmission.h"
#include "FfmpegProcess.h"
#include "MediaProbe.h"
#include "VodEncodeStream.h"
#include "ChannelPlaylistSplicer.h"
#include "cache/SegmentCache.h"
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

// Live-channel HLS segment length. Header-scope (not file-local in
// ChannelSession.cpp, where this used to live) because CacheSizing.cpp and
// Router.cpp's segment-cache wiring both need it too and must never drift
// from the value ffmpeg's own -hls_time/-force_key_frames actually use —
// see appendOutputArgs' own comment in ChannelSession.cpp for why 2 vs. 6 is
// a real, previously-revisited tradeoff, not a settled value.
inline constexpr int kLiveHlsSegmentSecs = 2;

// How many of a live-channel bucket's segments the in-memory SegmentCache
// keeps resident, independent of the on-disk retention window
// (kLiveHlsListSize+kLiveHlsDeleteThreshold in ChannelSession.cpp, ~20
// segments/~40s) — live playback never seeks backward, so caching the full
// disk window would just waste RAM. Native (direct-stream) segments carry
// whatever the source's own bitrate is (uncapped, unlike the transcode
// bucket's -maxrate ceiling), so its cap is tighter.
inline constexpr size_t kLiveCacheMaxSegmentsDefault = 4;
inline constexpr size_t kLiveCacheMaxSegmentsNative  = 2;

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
	// Host-wide hardware-encode-session cap shared with VOD/preview — see
	// EncoderAdmission's own class comment. nullptr (default) disables
	// gating entirely, same as every other caller of it.
	EncoderAdmission* encoder_admission = nullptr;
	// In-memory segment cache shared with VOD/preview — nullptr (default)
	// disables caching entirely (matches SegmentCache's own budget=0
	// disabled state; this codebase always constructs one at process start,
	// see main.cpp, so nullptr in practice only happens in tests).
	SegmentCache* segment_cache = nullptr;
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

	// Distinguishes this ChannelSession instance from any previous or future
	// one for the same (channel_id, bucket) key — set once at construction,
	// never changes afterward. SessionManager::getOrCreate() silently
	// replaces an inactive session with a brand-new instance, which resets
	// segment numbering back to seg-00000.ts (ChannelPlaylistSplicer's own
	// next_seq_) while reusing the exact same on-disk hlsDir(). Hephaestus's
	// own SegmentCache is fine with that (stop() invalidates its cache
	// entries for hlsDir() on teardown) but Hermes's independent cache has
	// no visibility into a Hephaestus-side restart at all — instanceId()
	// lets the content-addressed segment URL (Router.cpp's bucket-explicit
	// route + the channel-viewer playlist rewrite) change across a restart,
	// so Hermes's cache naturally keys a fresh incarnation's segments
	// separately instead of risking a stale-content collision under a
	// reused seg-NNNNN.ts filename.
	int64_t instance_id;

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

	// ── New HLS-only pipeline: VOD-backed producer + ChannelPlaylistSplicer ──
	// Additive to the legacy MPEG-TS pipeline above (ffmpeg/onData/clients),
	// not a replacement — MPEG-TS/HDHomeRun clients already buffer/smooth
	// client-side and don't have the small-rolling-window content-skip
	// problem HLS has, so they stay on the untouched real-time -re-paced
	// path (including computeSpeed() below). This pipeline exists purely to
	// give Pantheon's own HLS clients (Hades web/mobile, Android, Roku,
	// Cast) gapless transitions — see project memory for the full design
	// history (a prior on-disk discontinuity-sequence patch this replaces
	// entirely, now that the splicer is canonical's sole, correct-by-
	// construction author).
	std::atomic<int> next_spawn_id_{0};
	std::string pendingDir(int spawn_id) const;

	std::unique_ptr<ChannelPlaylistSplicer> splicer_;

	// Deliberately reuses current_item/current_item_mtx/item_start/
	// current_item_offset_ms above as the one shared "what's on now" state
	// rather than tracking its own separate item — hlsProducerTick() writes
	// them the same way transition() does, so currentTitle()/
	// currentFilePath()/onItemPlaying need no changes regardless of which
	// pipeline is actually driving them (see plan doc: when opts.hls_root is
	// set, this pipeline is authoritative and the legacy one becomes purely
	// reactive — see addClient()/removeClient()/onExit()).
	std::mutex hls_mtx_; // guards every hls_* member below
	std::unique_ptr<VodEncodeStream> hls_producer_;
	std::unique_ptr<FfmpegProcess> hls_offline_ffmpeg_; // offline/splash producer — see hlsSpawnOfflineProducer()
	std::vector<int64_t> hls_segment_boundaries_ms_;    // 0-based, for whichever producer above is active
	int64_t hls_wall_clock_start_ms_ = 0;               // real time corresponding to offset 0 of the active spawn

	std::thread hls_producer_thread_;
	std::atomic<bool> hls_producer_stop_{false};
	void hlsProducerLoop();
	// Starts (or, mid-item, keeps producing ahead of) whatever HLS should be
	// airing right now. Resolves a fresh item via Kairos once current_item's
	// wall_clock_end_ms has passed (or none is set yet — same fallback
	// shape as transition()'s own "Kairos has nothing" branch), spawns a new
	// VodEncodeStream into its own pendingDir() and hands it to the
	// splicer; otherwise just drives the existing producer forward
	// (VodEncodeStream::prepareSegment()/tick(), mirroring VodSession's own
	// lookaheadLoop) so it stays ahead of what the splicer needs to reveal.
	// Note: unlike the legacy pipeline's start(), there's no fast-path/splash
	// distinction here for a slow initial Kairos response — the first tick
	// just blocks briefly; Router.cpp's existing waitForFile()+503 already
	// covers a client asking before that first tick lands.
	void hlsProducerTick();
	// item.keyframes_ms-based (native bucket) or uniform-cadence (default
	// bucket) segment boundaries, probes audio/subtitle tracks, spawns a
	// VodEncodeStream into a fresh pendingDir(), and hands it to the
	// splicer — the real-content path.
	void hlsSpawnProducer(const KairosNowResponse& item, int64_t startOffsetMs, int64_t wallClockStartMs);
	// Offline slate/no-schedule fallback — a single indefinitely-bounded
	// looped-image encode has no real segment-index/seek concept for
	// VodEncodeStream's head model to apply to, so this uses a plain
	// FfmpegProcess (mirroring spawnOffline()'s own buildImageArgs), just
	// writing into a pendingDir() like every other producer instead of
	// hlsDir() directly, with uniform boundaries the splicer can still page
	// through the same way. Never preroll'd — see hlsMaintainPrerollQueue()'s own
	// comment on why.
	void hlsSpawnOfflineProducer(const KairosNowResponse& item);

	// ── Preroll: eliminates hlsSpawnProducer()'s cold-start gap ────────────
	// A VodEncodeStream built ahead of time, kept aside rather than made
	// active — the actual fix this whole pipeline exists for. See project
	// memory / plan doc for the full design.
	struct HlsProducerHandle
	{
		std::unique_ptr<VodEncodeStream> producer;
		std::vector<int64_t> boundaries;
		std::string dir;
		std::optional<MediaInfo> info;
		int audio_track = 0;
		int64_t span_ms = 0;
	};

	// Builds (doesn't activate) a producer for `item` starting at
	// startOffsetMs within it — the part of hlsSpawnProducer() that doesn't
	// depend on *when* it runs, shared by the reactive path (promotes
	// immediately) and hlsMaintainPrerollQueue() below (stashes the result until the
	// real transition moment, promoting it only if the item Kairos resolves
	// then still matches).
	HlsProducerHandle hlsCreateProducer(const KairosNowResponse& item, int64_t startOffsetMs);
	// Makes `handle` the active producer for `item`: current_info/
	// onItemPlaying (must only fire once an item is actually on air, not at
	// preroll time), swaps it into hls_producer_, hands it to the splicer.
	void hlsPromoteProducer(const KairosNowResponse& item, HlsProducerHandle handle, int64_t wallClockStartMs);

	// Once inside kPrefetchLeadMs of cur's own end, walks the schedule
	// forward from wherever the queue currently leaves off and builds (but
	// doesn't activate) a producer for every item whose own start falls
	// within that same lead window from cur's end — not just the single
	// next item. A run of short filler/bumpers packed inside the window
	// (e.g. a 5s bumper then a 30s filler, both starting well inside an 8s
	// lookahead from the outgoing episode's end) all get their producers
	// building in parallel from the same moment, rather than the filler
	// only starting to build once the bumper it follows actually becomes
	// current — that reactive, one-at-a-time chaining is what left barely
	// any lead time for whichever item followed a very short one. Offline
	// items are never preroll'd (see hlsSpawnOfflineProducer()) and stop
	// the walk — hlsProducerTick() falls back to spawning one reactively
	// same as today whenever what's actually next turns out to be offline.
	// Bounded by kMaxHlsPrerollQueueDepth regardless of how much of the
	// window that leaves uncovered.
	void hlsMaintainPrerollQueue(const KairosNowResponse& cur, int64_t now);
	// item_id (when either side has one) or file_path otherwise — same
	// identity Kairos's own deterministic schedule would resolve twice in a
	// row unless something actually changed (an admin edit, a re-sync)
	// between the preroll check and the real transition.
	static bool hlsSameItem(const KairosNowResponse& a, const KairosNowResponse& b);

	// One queued-ahead producer plus enough of its own KairosNowResponse to
	// promote or stale-check it later without re-resolving from Kairos.
	struct PrerollEntry
	{
		KairosNowResponse item;
		HlsProducerHandle handle;
		int64_t wall_clock_start_ms = 0;
	};

	// Always in schedule order (oldest/soonest-airing first) — the walk in
	// hlsMaintainPrerollQueue() only ever appends, and promotion/staleness
	// only ever removes from the front, so nothing later in the deque can
	// be stale (or due for promotion) before something earlier in it is.
	std::deque<PrerollEntry> hls_preroll_queue_;

	// Spawns the legacy MPEG-TS pipeline for whatever current_item currently
	// says, at however far into it "now" actually is — used by addClient()'s
	// 0->1 transition (lazy start, see plan doc) and by onExit() to respawn
	// across an item boundary while clients are still connected, instead of
	// each doing its own independent Kairos resolution.
	void spawnLegacyForCurrentItem();

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

	// The preroll mismatch-fallback safety property (hlsMaintainPrerollQueue()'s own
	// comment) rests entirely on this — worth testing in isolation.
	static bool hlsSameItemForTest(const KairosNowResponse& a, const KairosNowResponse& b)
	{
		return hlsSameItem(a, b);
	}

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
	// Live playlist.m3u8 with the discontinuity-sequence tag patched in
	// memory — serve this instead of the raw file. nullopt if not readable.
	std::optional<std::string> playlistForClient() const;

	bool isActive() const { return active.load(); }
	const std::string& channelId() const { return channel_id; }
	const std::string& bucketName() const { return bucket; }
	// See instance_id's own comment (below, private section) for why this
	// exists and what it's for.
	int64_t instanceId() const { return instance_id; }

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