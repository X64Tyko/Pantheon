#include "PlaybackHistoryRepository.h"
#include "Database.h"
#include "DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <iostream>

PlaybackHistoryRepository::PlaybackHistoryRepository(Database& db) : db_(db) {}

void PlaybackHistoryRepository::recordPing(const std::string& user_id, const std::string& content_type,
                                            const std::string& content_id, const std::string& title,
                                            const std::string& device_type, bool direct_play,
                                            int64_t position_ms, int64_t duration_ms, int64_t now_ms,
                                            bool completed) {
	// Best-effort by design (see header comment) — a failure here must never
	// propagate up and fail the watch-progress ping itself.
	try {
		SQLite::Statement find(db_.get(), R"SQL(
			SELECT event_id FROM playback_history
			WHERE user_id = ? AND content_type = ? AND content_id = ? AND ended_at_ms >= ?
			ORDER BY ended_at_ms DESC LIMIT 1
		)SQL");
		find.bind(1, user_id);
		find.bind(2, content_type);
		find.bind(3, content_id);
		find.bind(4, now_ms - kSessionGapMs);

		if (find.executeStep()) {
			std::string event_id = find.getColumn(0).getString();
			SQLite::Statement upd(db_.get(), R"SQL(
				UPDATE playback_history SET
					title = ?, device_type = ?, direct_play = ?,
					ended_at_ms = ?, last_position_ms = ?, duration_ms = ?,
					completed = completed OR ?
				WHERE event_id = ?
			)SQL");
			upd.bind(1, title);
			upd.bind(2, device_type);
			upd.bind(3, direct_play ? 1 : 0);
			upd.bind(4, now_ms);
			upd.bind(5, position_ms);
			upd.bind(6, duration_ms);
			upd.bind(7, completed ? 1 : 0);
			upd.bind(8, event_id);
			upd.exec();
		} else {
			SQLite::Statement ins(db_.get(), R"SQL(
				INSERT INTO playback_history
					(event_id, user_id, content_type, content_id, title, device_type, direct_play,
					 started_at_ms, ended_at_ms, started_position_ms, last_position_ms, duration_ms, completed)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
			)SQL");
			ins.bind(1,  db::generateId());
			ins.bind(2,  user_id);
			ins.bind(3,  content_type);
			ins.bind(4,  content_id);
			ins.bind(5,  title);
			ins.bind(6,  device_type);
			ins.bind(7,  direct_play ? 1 : 0);
			ins.bind(8,  now_ms);
			ins.bind(9,  now_ms);
			ins.bind(10, position_ms);
			ins.bind(11, position_ms);
			ins.bind(12, duration_ms);
			ins.bind(13, completed ? 1 : 0);
			ins.exec();
		}
	} catch (const std::exception& e) {
		std::cerr << "[playback_history] recordPing failed: " << e.what() << "\n";
	}
}

namespace {
// Shared by list() and listActive() — both select the exact same column set
// in the exact same order, just with different WHERE/ORDER BY clauses.
PlaybackHistoryRow rowFromStatement(SQLite::Statement& q) {
	PlaybackHistoryRow r;
	r.event_id             = q.getColumn(0).getString();
	r.user_id              = q.getColumn(1).getString();
	r.content_type         = q.getColumn(2).getString();
	r.content_id           = q.getColumn(3).getString();
	r.title                = q.getColumn(4).getString();
	r.device_type          = q.getColumn(5).getString();
	r.direct_play          = q.getColumn(6).getInt() != 0;
	r.started_at_ms        = q.getColumn(7).getInt64();
	r.ended_at_ms           = q.getColumn(8).getInt64();
	r.started_position_ms  = q.getColumn(9).getInt64();
	r.last_position_ms     = q.getColumn(10).getInt64();
	r.duration_ms           = q.getColumn(11).getInt64();
	r.completed             = q.getColumn(12).getInt() != 0;
	return r;
}

constexpr const char* kSelectColumns = R"SQL(
	SELECT event_id, user_id, content_type, content_id, title, device_type, direct_play,
	       started_at_ms, ended_at_ms, started_position_ms, last_position_ms, duration_ms, completed
	FROM playback_history
)SQL";
} // namespace

std::vector<PlaybackHistoryRow> PlaybackHistoryRepository::list(const std::string& user_id_filter,
                                                                  int64_t from_ms, int64_t to_ms, int limit) {
	std::vector<PlaybackHistoryRow> out;

	std::string sql = std::string(kSelectColumns) + " WHERE 1=1";
	if (!user_id_filter.empty()) sql += " AND user_id = ?";
	if (from_ms > 0) sql += " AND started_at_ms >= ?";
	if (to_ms   > 0) sql += " AND started_at_ms <= ?";
	sql += " ORDER BY started_at_ms DESC LIMIT ?";

	SQLite::Statement q(db_.get(), sql);
	int idx = 1;
	if (!user_id_filter.empty()) q.bind(idx++, user_id_filter);
	if (from_ms > 0) q.bind(idx++, from_ms);
	if (to_ms   > 0) q.bind(idx++, to_ms);
	q.bind(idx++, limit);

	while (q.executeStep()) out.push_back(rowFromStatement(q));
	return out;
}

std::vector<PlaybackHistoryRow> PlaybackHistoryRepository::listActive(int64_t since_ms) {
	std::vector<PlaybackHistoryRow> out;
	std::string sql = std::string(kSelectColumns) + " WHERE ended_at_ms >= ? ORDER BY ended_at_ms DESC";
	SQLite::Statement q(db_.get(), sql);
	q.bind(1, since_ms);
	while (q.executeStep()) out.push_back(rowFromStatement(q));
	return out;
}

int PlaybackHistoryRepository::prune(int64_t max_age_ms, int64_t now_ms) {
	SQLite::Statement del(db_.get(), "DELETE FROM playback_history WHERE started_at_ms < ?");
	del.bind(1, now_ms - max_age_ms);
	del.exec();
	return db_.get().getChanges();
}
