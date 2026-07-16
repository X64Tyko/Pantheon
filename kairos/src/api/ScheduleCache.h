#pragma once
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>

class Database;

class ScheduleCache {
public:
	explicit ScheduleCache(Database& db);

	// Delete all cached schedule rows for a channel and any future is_scheduled=1
	// play_history entries, then evict the in-memory preview cache entry.
	void clear(const std::string& channel_id);

	// Everything clear() does, plus wipes persisted CursorState (media_cursor/
	// block_state) and the RNG/cursor anchor snapshot (channel.anchor_hashes).
	// Needed when a structural change (priority, windowing, cursor_scope,
	// no_history_behavior, play_style, advancement) makes old accumulated cursor
	// state meaningless or actively wrong for the new configuration — clear()
	// alone would still resume projection from that stale state.
	void hardReset(const std::string& channel_id);

	// In-memory preview cache — keyed by channel_id, valid for one (seed, week_anchor) pair.
	bool  getPreview(const std::string& channel_id, int seed, std::time_t anchor,
	                 std::string& out);
	void  setPreview(const std::string& channel_id, int seed, std::time_t anchor,
	                 const std::string& body);
	void  evictPreview(const std::string& channel_id);

private:
	struct Entry { int seed; std::time_t anchor; std::string body; };

	Database&                                    db_;
	std::mutex                                   mu_;
	std::unordered_map<std::string, Entry>       cache_;
};
