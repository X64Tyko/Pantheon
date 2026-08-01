#pragma once
#include "FfmpegProcess.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// One elementary HLS stream (video-only or audio-only) within a VOD
// session's directory, covered by a small set of bounded-window "heads" —
// each a real ffmpeg process responsible for a fixed span of segments
// (kVodHeadWindowSegments), not the whole remaining file. Pulled out of
// VodSession (which used to own exactly one long-lived, indefinitely
// paused/resumed process directly, for a single combined video+audio
// stream) for two reasons:
//
// 1. Splitting video and audio into independent streams is what lets an
//    audio-track switch stop requiring a full video re-encode, and is the
//    foundation shared/cross-viewer VOD sessions build on — see
//    VodSession.h's class comment.
// 2. The multi-head design (same shape as Kyoo's own transcoder — see the
//    "watch together" design thread this came out of) is what makes seeking
//    back into recently-played territory free (the head covering it is
//    still there, at worst paused — no kill, no restart, no wait) and is
//    *required*, not just nice-to-have, once a stream is shared across
//    viewers sitting far apart in the same file: a single process would
//    otherwise have its progress evicted every time either viewer moved,
//    each restart undoing the other's runway.
//
// A live head — even a paused one — still holds its NVENC/VAAPI slot
// (FfmpegProcess::pause()'s own doc comment), so the number of concurrently
// *live* heads (not just running ones) is capped; a request that needs a
// new head once already at the cap evicts whichever live head was least
// recently requested, via the same spawn-before-kill overlap
// VodSession::restartAt() already used for its single-process model (spawn
// the replacement, confirm it didn't immediately die, only then kill the
// evictee) so a legitimate new-territory seek never pays the evictee's
// teardown wait serially.
class VodEncodeStream
{
public:
	// argsBuilder(segment_index, position_ms, window_duration_secs) -> full
	// ffmpeg argv for a head starting at that segment/position. Owns
	// knowing whether this is a video-only or audio-only invocation,
	// subtitle burn-in, HDR, etc. — including where a `-t <window_duration_
	// secs>` output-duration bound belongs in the argv (ffmpeg is picky
	// about option ordering, so this class only supplies the value, never
	// splices it into an argv it didn't build). nullopt when this head runs
	// to the real end of the file (its window reaches the last segment) —
	// no explicit bound needed there, real EOF already terminates it.
	using ArgsBuilder = std::function<std::vector<std::string>(
        int segment_index, int64_t position_ms, std::optional<double> window_duration_secs
	)
	>;

	// label: "video" | "audio" — log-line prefix only. segment_dir: the VOD
	// session's directory (shared by both streams). segment_prefix:
	// filename prefix distinguishing this stream's segments from the other
	// one's in the same directory (e.g. "seg-" vs "aseg-"). lookahead_secs/
	// hls_time_secs mirror VodStreamOptions' own fields — passed in rather
	// than read from a shared opts struct so this class stays independent of
	// VodSession's own config plumbing.
	VodEncodeStream(std::string label, std::string segment_dir, std::string segment_prefix,
					ArgsBuilder argsBuilder, int buffer_size, bool ffmpeg_debug_logs, bool verbose_transcode_logs,
					int lookahead_secs, int hls_time_secs);
	~VodEncodeStream();

	VodEncodeStream(const VodEncodeStream&)            = delete;
	VodEncodeStream& operator=(const VodEncodeStream&) = delete;

	enum class SegmentPrep { Ready, WaitShort, WaitColdStart, Failed };

	// Router-facing decision for a requested segment index — same contract
	// VodSession::prepareSegment() used to implement directly, now resolved
	// against whichever head (existing or freshly spawned) covers this
	// index rather than a single process.
	SegmentPrep prepareSegment(int segment_index, const std::vector<int64_t>& segment_start_ms, int total_segments);

	// Periodic tick (VodSession's existing lookahead thread drives this,
	// same ~2s cadence as before) — scans on-disk progress and pauses/
	// resumes each live head independently, based on how far ITS OWN
	// generated frontier has gotten ahead of ITS OWN last-requested segment.
	// Also reaps heads that finished their window and have gone idle.
	void tick(int total_segments);

	// Kills every live head and stops accepting further ones — called once
	// by VodSession::stop().
	void stop();

	void setVerboseTranscodeLogs(bool v) { verbose_transcode_logs_ = v; }
	void setBufferSize(int v) { buffer_size_ = v; }

	// Public so Router.cpp can resolve the on-disk path for a segment
	// directly off a shared VodEncodeStream (via VodSession's own
	// accessors) — segments live under this stream's own directory now,
	// not any single viewer's session_id (see VodSessionManager.h's class
	// comment on why streams are shared/keyed by content instead).
	std::string segmentPath(int index) const;

	// Debug/activity-view accessors (ActivityRouter) — briefly locked (cheap,
	// low-contention periodic poll), staleness tolerance only in the sense
	// that "paused right now" can flip a moment after this returns, same as
	// VodSession's own isMainEncoderPaused() always tolerated.
	bool anyHeadPaused() const;
	int liveHeadCount() const;
	// Highest generated segment across all live heads — best single scalar
	// for a debug view; not used for any real prepareSegment()/tick() logic,
	// which is all per-head internally.
	int highestGeneratedSegment() const;

private:
	struct Head
	{
		std::unique_ptr<FfmpegProcess> ffmpeg;
		int start_segment      = 0;
		int window_end_segment = 0; // exclusive
		std::atomic<int> highest_generated{-1};
		std::atomic<int> last_requested{-1};
		std::atomic<int64_t> last_requested_at_ms{0};
		// shared_ptr, not a plain atomic<bool>: FfmpegProcess's on_exit fires
		// asynchronously on a TaskRegistry thread (FfmpegProcess.cpp), which
		// can run after this Head has already been erased from heads_ (e.g.
		// reaped by tick() the moment its window finished). The exit
		// callback captures a copy of this shared_ptr, not `this`/a raw
		// Head*, so it always has something live to write to regardless of
		// whether the Head itself still exists by the time it runs.
		std::shared_ptr<std::atomic<bool>> exited_naturally = std::make_shared<std::atomic<bool>>(false);
	};

	// All require mtx_ already held by the caller. graveyard collects heads
	// evicted/removed during the call instead of killing them inline — mtx_
	// is shared across every viewer of this content (VodSessionManager keys
	// streams by content, not by viewer), and FfmpegProcess::kill()/
	// ~FfmpegProcess() blocks until the process is confirmed dead, so killing
	// while still holding mtx_ would stall every other concurrent viewer's
	// prepareSegment()/tick() call on this same stream for however long that
	// takes. Callers must declare graveyard before (so it's destructed, and
	// only then blocks on each evicted head's teardown, after) the mtx_ lock
	// guard's own scope ends — see prepareSegment()'s own comment for the
	// exact pattern.
	Head* findCoveringHead(int segment_index);
	Head* spawnHead(int segment_index, int64_t position_ms, const std::vector<int64_t>& segment_start_ms,
					int total_segments, std::vector<std::unique_ptr<Head>>& graveyard);
	void evictOneIfAtCap(std::vector<std::unique_ptr<Head>>& graveyard);

	std::string label_;
	std::string segment_dir_;
	std::string segment_prefix_;
	ArgsBuilder args_builder_;
	int buffer_size_;
	bool ffmpeg_debug_logs_;
	bool verbose_transcode_logs_;
	int lookahead_secs_;
	int hls_time_secs_;

	mutable std::mutex mtx_;
	std::vector<std::unique_ptr<Head>> heads_; // guarded by mtx_; dead heads erased, not just marked
};