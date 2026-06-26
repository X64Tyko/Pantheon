#include "ScheduleCache.h"
#include "../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>

ScheduleCache::ScheduleCache(Database& db) : db_(db) {}

void ScheduleCache::clear(const std::string& channel_id) {
	auto now = static_cast<int64_t>(std::time(nullptr));

	SQLite::Statement d1(db_.get(),
		"DELETE FROM scheduled_program WHERE channel_id = ?");
	d1.bind(1, channel_id);
	d1.exec();

	SQLite::Statement d2(db_.get(),
		"DELETE FROM play_history WHERE channel_id = ? AND is_scheduled = 1 AND aired_at >= ?");
	d2.bind(1, channel_id);
	d2.bind(2, now);
	d2.exec();

	evictPreview(channel_id);
}

bool ScheduleCache::getPreview(const std::string& channel_id, int seed,
                                std::time_t anchor, std::string& out) {
	std::lock_guard<std::mutex> lk(mu_);
	auto it = cache_.find(channel_id);
	if (it == cache_.end()) return false;
	if (it->second.seed != seed || it->second.anchor != anchor) return false;
	out = it->second.body;
	return true;
}

void ScheduleCache::setPreview(const std::string& channel_id, int seed,
                                std::time_t anchor, const std::string& body) {
	std::lock_guard<std::mutex> lk(mu_);
	cache_[channel_id] = { seed, anchor, body };
}

void ScheduleCache::evictPreview(const std::string& channel_id) {
	std::lock_guard<std::mutex> lk(mu_);
	cache_.erase(channel_id);
}
