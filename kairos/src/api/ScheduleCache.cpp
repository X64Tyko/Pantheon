#include "ScheduleCache.h"
#include "../db/CursorRepository.h"
#include "../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <ctime>

ScheduleCache::ScheduleCache(Database& db)
	: db_(db)
{
}

void ScheduleCache::clear(const std::string& channel_id)
{
	auto now = static_cast<int64_t>(std::time(nullptr));

	// Preserve whatever's actually on-air right now (wall_clock_start <= now <
	// wall_clock_end) instead of wiping it along with every other row —
	// Hephaestus's ChannelSession is actively streaming that exact item/timing
	// via /api/channels/:id/now, and deleting it out from under a live edit
	// makes the next poll either find nothing (stream cut) or re-resolve to a
	// different item/offset under the freshly-edited blocks (a jarring
	// mid-playback switch). Rows that haven't started yet still get wiped
	// unconditionally, which is exactly what should pick up the edit.
	SQLite::Statement d1(db_.get(),
						 "DELETE FROM scheduled_program WHERE channel_id = ? "
						 "AND NOT (wall_clock_start <= ? AND wall_clock_end > ?)");
	d1.bind(1, channel_id);
	d1.bind(2, now);
	d1.bind(3, now);
	d1.exec();

	SQLite::Statement d2(db_.get(),
						 "DELETE FROM play_history WHERE channel_id = ? AND is_scheduled = 1 AND aired_at >= ?");
	d2.bind(1, channel_id);
	d2.bind(2, now);
	d2.exec();

	evictPreview(channel_id);
}

void ScheduleCache::hardReset(const std::string& channel_id)
{
	clear(channel_id);

	CursorRepository(db_).clear(channel_id);

	SQLite::Statement upd(db_.get(),
						  "UPDATE channel SET anchor_hashes = NULL WHERE channel_id = ?");
	upd.bind(1, channel_id);
	upd.exec();
}

bool ScheduleCache::getPreview(const std::string& channel_id, int seed,
							   std::time_t anchor, std::string& out)
{
	std::lock_guard<std::mutex> lk(mu_);
	auto it = cache_.find(channel_id);
	if (it == cache_.end()) return false;
	if (it->second.seed != seed || it->second.anchor != anchor) return false;
	out = it->second.body;
	return true;
}

void ScheduleCache::setPreview(const std::string& channel_id, int seed,
							   std::time_t anchor, const std::string& body)
{
	std::lock_guard<std::mutex> lk(mu_);
	cache_[channel_id] = {seed, anchor, body};
}

void ScheduleCache::evictPreview(const std::string& channel_id)
{
	std::lock_guard<std::mutex> lk(mu_);
	cache_.erase(channel_id);
}