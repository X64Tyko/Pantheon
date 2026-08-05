#pragma once
#include <atomic>

class Database;
class ConfStore;
class SyncManager;

// Re-encodes non-H.264/AAC or VFR-flagged episode/movie files to H.264/AAC
// CFR in place, so more of the library qualifies for Hephaestus's cheap
// direct-stream channel bucket instead of a live software transcode (see
// isLikelyVfr/isChannelDirectStreamable there). Single-flight and
// sequential — unlike ChapterDetectionManager's worker pool, a single
// libx264 encode already saturates every core on its own, so running
// several in parallel would only add contention, not throughput.
class MediaNormalizeManager
{
public:
	MediaNormalizeManager(Database& db, ConfStore& conf, SyncManager& sync);

	// False (no-op) if a sweep is already running. Whole-library, manual
	// trigger only (no scheduled/automatic run) — see JobService.
	bool triggerNormalize();
	bool isRunning() const { return running_.load(); }

private:
	void run();

	Database& db_;
	ConfStore& conf_;
	SyncManager& sync_;
	std::atomic<bool> running_{false};
};