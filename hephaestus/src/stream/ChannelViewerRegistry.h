#pragma once
#include "ClientCapabilities.h"
#include "MediaProbe.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class SessionManager;

// Combined video+audio copy-eligibility check for a channel's currently-
// playing item against one viewer's declared capabilities — the bucket
// decision for the two-bucket model (native=copy, default=transcode).
// Combined, not independent per-track like VOD's own
// isVideoDirectStreamable/isAudioDirectStreamable (VodSession.cpp), to keep
// the bucket space binary — see the plan's "two buckets" scope decision.
bool isChannelDirectStreamable(const MediaInfo& info, int audio_track, const ClientCapabilities& caps);

// Per-viewer registry for capability-bucketed live-channel HLS sessions.
// HLS only — MPEG-TS/HDHomeRun stays on the single legacy per-channel
// session forever, untouched by any of this (see the plan's scope
// decisions: ChannelBroadcaster's single shared upstream connection per
// channel has no per-viewer identity to hang a bucket assignment off of).
//
// Tracks which bucket (ChannelSession::kNativeBucket/kDefaultBucket) each
// identified viewer is PINNED to for the life of their connection — set once
// at start() and never silently changed thereafter (see ViewerEntry::bucket's
// own comment for why: an original version of this class let
// reassignForChannel() migrate an already-connected viewer's serving bucket
// live, on the theory that a viewer's next playlist/segment poll would just
// transparently resolve to wherever they're newly assigned, no client-visible
// URL change or reconnect needed. In production that "transparently" was the
// bug: default and native are two fully independent ffmpeg processes/HLS
// playlists with unrelated segment numbering and no shared timeline, and
// hls.js has no way to know the manifest URL it's been polling just started
// resolving to a different one out from under it — the observed symptom was
// literally one item's audio/video bleeding into the next at every bucket
// flip, plus stalls at each one. reassignForChannel now only ever updates
// recommended_bucket (advisory) — see its own comment; Hades polls
// GET /stream/channel/viewer/:id/status and does a real reconnect (a fresh
// start(), which re-pins to whatever's ideal *right now*) when it sees the
// recommendation diverge from what it's pinned to.
class ChannelViewerRegistry
{
public:
	explicit ChannelViewerRegistry(SessionManager& sessions);
	~ChannelViewerRegistry();

	struct StartResult
	{
		std::string viewer_session_id;
		std::string bucket;
	};

	// Registers a new viewer, resolving their initial (pinned) bucket
	// against the channel's currently-playing item (info == nullptr when
	// unknown/offline — always resolves to the default bucket in that case).
	// Spins up the native bucket's ChannelSession immediately if eligible,
	// via SessionManager::getOrCreate (a no-op if it's already running).
	StartResult start(const std::string& channel_id, const ClientCapabilities& caps,
					  const MediaInfo* info, int audio_track);

	// Resolves a viewer's current (pinned) bucket for a follow-up HLS request
	// and refreshes their last-seen time. Returns "" if the viewer_session_id
	// is unknown (never started, or reaped as idle) — caller should treat
	// that as 404. Writes the viewer's channel_id into channel_id_out.
	std::string touch(const std::string& viewer_session_id, std::string& channel_id_out);

	// Explicit client-side cleanup (POST /stream/channel/viewer/:id/stop).
	void stop(const std::string& viewer_session_id);

	struct ViewerStatus
	{
		std::string bucket;             // pinned — what's actually being served
		std::string recommended_bucket; // advisory — what a fresh reconnect would resolve to now
	};

	// Read-only lookup for GET /stream/channel/viewer/:id/status (Hades polls
	// this to decide whether to reconnect — see this class's own comment).
	// Side-effect-free: doesn't refresh last_seen_ms, unlike touch() — the
	// much more frequent playlist/segment polls already keep idle-reaping
	// accurate, so this doesn't need to duplicate that.
	std::optional<ViewerStatus> status(const std::string& viewer_session_id) const;

	// Recomputes the *recommended* bucket for every registered viewer of
	// `channel_id` against the newly-playing item — never touches any
	// viewer's pinned serving bucket. The opportunistic default->native
	// upgrade is gated on the item being substantial enough that a reconnect
	// is actually worth it (see kMinReassignDurationMs in the .cpp); a
	// native->default *safety* fallback (a viewer pinned to native who's no
	// longer eligible for what's now airing) is never gated — see the .cpp's
	// own comment on why that direction can't wait. Also opportunistically
	// pre-warms whichever bucket(s) viewers now want, so a reconnect that
	// acts on the recommendation finds it already running instead of paying
	// a cold spin-up. Idempotent — safe to call redundantly from more than
	// one bucket instance's own transition for the same item, since both
	// resolve the same deterministic Kairos schedule.
	void reassignForChannel(const std::string& channel_id, const std::optional<MediaInfo>& info, int audio_track,
							int64_t item_duration_ms, bool is_filler);

	// Exact per-channel, per-bucket viewer counts, for the activity/debugging
	// view (ActivityRouter) — the one place this registry's identified
	// viewers give a real count instead of the legacy MPEG-TS path's
	// ClientSink-based client_count (exact but bucket-blind on its own) or
	// HLS's own presence-only hlsViewerActive signal.
	std::map<std::string, std::map<std::string, int>> viewerCounts() const;

private:
	struct ViewerEntry
	{
		std::string channel_id;
		ClientCapabilities caps;
		// Pinned at start() (or a subsequent reconnect, which is really just a
		// fresh start() under a new viewer_session_id) — see this class's own
		// comment for why reassignForChannel() must never mutate this after
		// the fact.
		std::string bucket;
		// Advisory only — see reassignForChannel()/status()'s own comments.
		// Seeded equal to `bucket` at start() so a viewer who never sees an
		// item transition (e.g. a short-lived connection) never reports a
		// spurious mismatch.
		std::string recommended_bucket;
		int64_t last_seen_ms;
	};

	SessionManager& sessions_;

	mutable std::mutex mtx_;                     // viewerCounts() is const and still needs to lock this
	std::map<std::string, ViewerEntry> viewers_; // key: viewer_session_id

	// A viewer who stops polling without an explicit /stop (app kill,
	// network loss) is dropped after being idle this long — mirrors
	// VodSessionManager's own idle-session sweep.
	std::atomic<bool> stop_reaper_{false};
	std::thread reaper_thread_;
	void reapLoop();
};