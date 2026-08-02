#pragma once
#include "ChannelSession.h"
#include "../kairos/KairosClient.h"
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#include <atomic>
#include <thread>

class ChannelViewerRegistry;

class SessionManager
{
	KairosClient& kairos;
	std::string ffmpeg_path;
	StreamOptions stream_opts;

	// Optional — wired via setViewerRegistry() from main.cpp once a
	// ChannelViewerRegistry exists (it needs a reference back to this
	// SessionManager, so it's constructed after). nullptr is harmless: live-
	// channel capability bucketing simply never engages, every session
	// behaves exactly as it did before that feature existed.
	ChannelViewerRegistry* viewer_registry_ = nullptr;

	std::mutex mtx;
	// Keyed by (channel_id, bucket) — see ChannelSession::kDefaultBucket/
	// kNativeBucket. Every pre-existing caller passes no bucket (defaults to
	// kDefaultBucket), so this is purely additive: the legacy single-
	// session-per-channel behavior is exactly the "default" bucket now.
	std::map<std::pair<std::string, std::string>, std::shared_ptr<ChannelSession>> sessions;

	// Channel list / stream buffer size, refreshed periodically in the
	// background instead of fetched from Kairos on every getOrCreate() call —
	// see the comment on kCacheRefreshInterval in SessionManager.cpp for why.
	std::mutex cache_mtx;
	std::vector<KairosChannel> cached_channels;
	int cached_buffer_size = 0;
	// nullopt = not yet fetched (or Kairos unreachable) — keep whatever
	// Config/--verbose-transcode/HEPH_VERBOSE_TRANSCODE set at startup.
	std::optional<bool> cached_verbose_transcode_logs;
	// Same shape, for ffmpeg_debug_logs (was HEPH_FFMPEG_DEBUG, env-only
	// until this) — see KairosClient::getFfmpegDebugLogs()'s own comment.
	std::optional<bool> cached_ffmpeg_debug_logs;
	std::atomic<bool> stop_refresh{false};
	std::thread refresh_thread;

	void refreshCache();

public:
	SessionManager(KairosClient& kairos, std::string ffmpeg_path, StreamOptions opts);
	~SessionManager();

	// Returns an active session for (channelId, bucket), creating and
	// starting one if needed. Returns nullptr if Kairos rejects the channel
	// or ffmpeg won't start. bucket defaults to the legacy universal
	// transcode session — every pre-existing caller is unaffected.
	std::shared_ptr<ChannelSession> getOrCreate(const std::string& channelId,
												const std::string& bucket = ChannelSession::kDefaultBucket);

	// Look-without-touch counterpart to getOrCreate() — returns an already-
	// running session for (channelId, bucket), or nullptr. Never spawns one.
	// Used where a caller wants to reuse whatever's already active instead of
	// forcing a fresh (and possibly redundant) session into existence just to
	// ask it a question.
	std::shared_ptr<ChannelSession> peek(const std::string& channelId,
										 const std::string& bucket = ChannelSession::kDefaultBucket);

	// Channel-level escape hatch from the two-bucket model (channel.force_transcode
	// in Kairos) — true means this channel's viewers should never be resolved
	// onto the native/direct-stream bucket at all, regardless of capability
	// eligibility (see ChannelViewerRegistry's use of this). Reads the same
	// periodically-refreshed cache getOrCreate() already applies per-channel
	// settings from; false (including "unknown channel") is the safe default.
	bool channelForcesTranscode(const std::string& channelId);

	// Remove sessions that have been stopped.
	void reap();

	// Snapshot of currently-active sessions, for the activity/debugging view
	// (ActivityRouter). Copies the shared_ptrs out under the lock rather
	// than returning a reference, so the caller can inspect them without
	// holding mtx.
	std::vector<std::shared_ptr<ChannelSession>> listActive();

	// Wires this manager to notify `registry` whenever any session it
	// creates resolves a new item — see ChannelSession::onItemPlaying and
	// ChannelViewerRegistry::reassignForChannel. Called once from main.cpp.
	void setViewerRegistry(ChannelViewerRegistry* registry) { viewer_registry_ = registry; }
};