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
