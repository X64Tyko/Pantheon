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
// identified viewer is currently assigned to. reassignForChannel() is
// invoked from ChannelSession's own spawn path (via its onItemPlaying
// callback, wired in SessionManager::getOrCreate) every time a new item
// starts playing on a channel, and keeps assignments current as the
// schedule advances — a viewer's next playlist/segment poll (touch())
// transparently resolves to whichever bucket they're now assigned to, with
// no client-visible URL change or reconnect.
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

	// Registers a new viewer, resolving their initial bucket against the
	// channel's currently-playing item (info == nullptr when unknown/
	// offline — always resolves to the default bucket in that case). Spins
	// up the native bucket's ChannelSession immediately if eligible, via
	// SessionManager::getOrCreate (a no-op if it's already running).
	StartResult start(const std::string& channel_id, const ClientCapabilities& caps,
					  const MediaInfo* info, int audio_track);

	// Resolves a viewer's current bucket for a follow-up HLS request and
	// refreshes their last-seen time. Returns "" if the viewer_session_id is
	// unknown (never started, or reaped as idle) — caller should treat that
	// as 404. Writes the viewer's channel_id into channel_id_out.
	std::string touch(const std::string& viewer_session_id, std::string& channel_id_out);

	// Explicit client-side cleanup (POST /stream/channel/viewer/:id/stop).
	void stop(const std::string& viewer_session_id);

	// Recomputes the ideal bucket for every registered viewer of
	// `channel_id` against the newly-playing item and updates any whose
	// ideal bucket changed. Idempotent — safe to call redundantly from more
	// than one bucket instance's own transition for the same item, since
	// both resolve the same deterministic Kairos schedule. Does not tear
	// down a now-empty bucket itself — that's ChannelSession's own existing
	// idle-linger machinery, which naturally kicks in once nothing touches
	// it anymore (see the plan's "reuse, don't rebuild" note).
	void reassignForChannel(const std::string& channel_id, const std::optional<MediaInfo>& info, int audio_track);

private:
	struct ViewerEntry
	{
		std::string channel_id;
		ClientCapabilities caps;
		std::string bucket;
		int64_t last_seen_ms;
	};

	SessionManager& sessions_;

	std::mutex mtx_;
	std::map<std::string, ViewerEntry> viewers_; // key: viewer_session_id

	// A viewer who stops polling without an explicit /stop (app kill,
	// network loss) is dropped after being idle this long — mirrors
	// VodSessionManager's own idle-session sweep.
	std::atomic<bool> stop_reaper_{false};
	std::thread reaper_thread_;
	void reapLoop();
};