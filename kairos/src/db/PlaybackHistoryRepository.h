#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Database;

// One "session" of watching a title — see migration v91's own comment for
// why this is a separate append-only table from watch_progress (which stays
// exactly what it's always been: current resume state, one row per
// (user,content), upserted in place).
struct PlaybackHistoryRow {
	std::string event_id;
	std::string user_id;
	std::string content_type;
	std::string content_id;
	std::string title;
	std::string device_type;   // "web" | "android-mobile" | "android-tv" | "roku" | "" (unknown)
	bool        direct_play = false;
	int64_t     started_at_ms = 0;
	int64_t     ended_at_ms   = 0;
	int64_t     started_position_ms = 0;
	int64_t     last_position_ms    = 0;
	int64_t     duration_ms  = 0;
	bool        completed    = false;
};

class PlaybackHistoryRepository {
public:
	explicit PlaybackHistoryRepository(Database& db);

	// Called from the same place watch_progress gets upserted (PUT
	// /api/watch-progress/:content_type/:id) — NOT a new event-reporting
	// pipeline, just piggybacking two more fields (device_type, direct_play)
	// onto pings the client already sends. Extends the most recent history
	// row for (user_id, content_type, content_id) if its ended_at_ms is
	// within kSessionGapMs of now_ms (still "the same sitting"); otherwise
	// starts a new row. Best-effort: swallows its own SQLite errors rather
	// than throwing, since a missed history row should never take down the
	// watch-progress ping itself (which remains the source of truth for
	// resume/continue-watching regardless of whether this succeeds).
	void recordPing(const std::string& user_id, const std::string& content_type,
	                 const std::string& content_id, const std::string& title,
	                 const std::string& device_type, bool direct_play,
	                 int64_t position_ms, int64_t duration_ms, int64_t now_ms, bool completed);

	// Most-recent-first. user_id_filter empty = every user (admin-only view —
	// callers must check role themselves, this repository has no concept of
	// permissions). from_ms/to_ms of 0 = unbounded on that side.
	std::vector<PlaybackHistoryRow> list(const std::string& user_id_filter,
	                                      int64_t from_ms, int64_t to_ms, int limit);

	// "Who's actively playing something right now" — unlike list(), which
	// windows on started_at_ms (when a sitting began), this windows on
	// ended_at_ms (when it was last pinged), since a session started an hour
	// ago but still pinging every ~15s is exactly the case this needs to
	// catch and list()'s own from/to would miss. since_ms is an absolute
	// timestamp (caller passes now_ms - some staleness budget, mirroring how
	// DeviceConnectionsPanel's STALE_MS works for Roku's own heartbeat) —
	// this repository has no wall-clock opinion of its own. Every device
	// type's presence (web/android/roku/cast) is derived from this same
	// watch-progress-ping data, not a separate heartbeat mechanism.
	std::vector<PlaybackHistoryRow> listActive(int64_t since_ms);

	// Deletes rows older than max_age_ms (relative to now_ms) — keeps this
	// table from growing forever on a long-running server. Returns the
	// number of rows removed. Call periodically (see ActivityService), not
	// on every write.
	int prune(int64_t max_age_ms, int64_t now_ms);

private:
	Database& db_;
	// How long a gap since the last ping still counts as "the same session"
	// — generous enough to survive a paused/thinking viewer or one dropped
	// ping (the player pings every ~15s), tight enough that reopening the
	// same title hours later is genuinely a new history entry.
	static constexpr int64_t kSessionGapMs = 15 * 60 * 1000;
};
