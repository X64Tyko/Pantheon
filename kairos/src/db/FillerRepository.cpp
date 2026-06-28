#include "FillerRepository.h"
#include "Database.h"
#include "DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>

FillerRepository::FillerRepository(Database& db) : db_(db) {}

std::string FillerRepository::create(const std::string& title, const std::string& advancement) {
	std::string fid = db::generateId();
	SQLite::Statement s(db_.get(),
		"INSERT INTO filler_list (filler_list_id, title, advancement) VALUES (?,?,?)");
	s.bind(1, fid); s.bind(2, title); s.bind(3, advancement);
	s.exec();
	return fid;
}

void FillerRepository::updateField(const std::string& filler_list_id,
                                    const std::string& col, const std::string& val) {
	SQLite::Statement s(db_.get(),
		"UPDATE filler_list SET " + col + " = ? WHERE filler_list_id = ?");
	s.bind(1, val); s.bind(2, filler_list_id); s.exec();
}

void FillerRepository::remove(const std::string& filler_list_id) {
	SQLite::Statement s(db_.get(), "DELETE FROM filler_list WHERE filler_list_id = ?");
	s.bind(1, filler_list_id); s.exec();
}

void FillerRepository::unlinkPlex(const std::string& filler_list_id) {
	SQLite::Statement s(db_.get(),
		"DELETE FROM plex_list_link WHERE list_type = 'filler_list' AND list_id = ?");
	s.bind(1, filler_list_id); s.exec();
}

std::pair<int64_t, int> FillerRepository::addItem(const std::string& filler_list_id,
                                                    const std::string& item_type,
                                                    const std::string& item_id) {
	int position = 0;
	{
		SQLite::Statement pq(db_.get(),
			"SELECT COALESCE(MAX(position), -1) + 1 FROM filler_list_item WHERE filler_list_id = ?");
		pq.bind(1, filler_list_id);
		if (pq.executeStep()) position = pq.getColumn(0).getInt();
	}
	SQLite::Statement s(db_.get(),
		"INSERT INTO filler_list_item (filler_list_id, item_type, item_id, position) VALUES (?,?,?,?)");
	s.bind(1, filler_list_id); s.bind(2, item_type); s.bind(3, item_id); s.bind(4, position);
	s.exec();
	return {db_.get().getLastInsertRowid(), position};
}

int FillerRepository::addItems(const std::string& filler_list_id,
                                const std::vector<std::pair<std::string, std::string>>& items) {
	SQLite::Statement pq(db_.get(),
		"SELECT COALESCE(MAX(position), -1) + 1 FROM filler_list_item WHERE filler_list_id = ?");
	pq.bind(1, filler_list_id);
	int position = pq.executeStep() ? pq.getColumn(0).getInt() : 0;
	int added = 0;
	SQLite::Transaction tx(db_.get());
	for (const auto& [item_type, item_id] : items) {
		if (item_id.empty()) continue;
		try {
			SQLite::Statement s(db_.get(),
				"INSERT INTO filler_list_item (filler_list_id, item_type, item_id, position) VALUES (?,?,?,?)");
			s.bind(1, filler_list_id); s.bind(2, item_type); s.bind(3, item_id); s.bind(4, position);
			s.exec();
			++position; ++added;
		} catch (const SQLite::Exception&) { /* skip duplicates */ }
	}
	tx.commit();
	return added;
}

void FillerRepository::removeItem(int item_id) {
	SQLite::Statement s(db_.get(), "DELETE FROM filler_list_item WHERE id = ?");
	s.bind(1, item_id); s.exec();
}

std::vector<FillerListRow> FillerRepository::listAll() {
	SQLite::Statement q(db_.get(), R"(
		SELECT fl.filler_list_id, fl.title, fl.advancement,
		       COUNT(fi.id) AS item_count,
		       COALESCE(SUM(CASE fi.item_type
		           WHEN 'episode' THEN e.duration_ms
		           WHEN 'movie'   THEN m.duration_ms ELSE 0 END), 0) AS total_ms,
		       pll.source_id, pll.external_id, pll.plex_type, pll.last_synced_at
		FROM filler_list fl
		LEFT JOIN filler_list_item fi ON fi.filler_list_id = fl.filler_list_id
		LEFT JOIN episode e ON fi.item_type = 'episode' AND fi.item_id = e.episode_id
		LEFT JOIN movie   m ON fi.item_type = 'movie'   AND fi.item_id = m.movie_id
		LEFT JOIN plex_list_link pll ON pll.list_type = 'filler_list' AND pll.list_id = fl.filler_list_id
		GROUP BY fl.filler_list_id ORDER BY fl.title
	)");
	std::vector<FillerListRow> rows;
	while (q.executeStep()) {
		FillerListRow r;
		r.filler_list_id = q.getColumn(0).getString();
		r.title          = q.getColumn(1).getString();
		r.advancement    = q.getColumn(2).getString();
		r.item_count     = q.getColumn(3).getInt();
		r.total_ms       = q.getColumn(4).getInt64();
		if (!q.getColumn(5).isNull()) {
			PlexLinkRow pl;
			pl.source_id   = q.getColumn(5).getString();
			pl.external_id = q.getColumn(6).getString();
			pl.plex_type   = q.getColumn(7).getString();
			if (!q.getColumn(8).isNull()) pl.last_synced_at = q.getColumn(8).getInt64();
			r.plex_link = std::move(pl);
		}
		rows.push_back(std::move(r));
	}
	return rows;
}

std::optional<FillerListDetail> FillerRepository::getDetail(const std::string& filler_list_id) {
	SQLite::Statement fh(db_.get(),
		"SELECT filler_list_id, title, advancement FROM filler_list WHERE filler_list_id = ?");
	fh.bind(1, filler_list_id);
	if (!fh.executeStep()) return std::nullopt;

	FillerListDetail d;
	d.filler_list_id = fh.getColumn(0).getString();
	d.title          = fh.getColumn(1).getString();
	d.advancement    = fh.getColumn(2).getString();

	SQLite::Statement q(db_.get(), R"(
		SELECT fi.id, fi.item_type, fi.item_id, fi.position,
		       CASE fi.item_type
		           WHEN 'episode' THEN s.title || ' S' || PRINTF('%02d',e.season) ||
		                               'E' || PRINTF('%02d',e.episode) || ' — ' || e.title
		           WHEN 'movie'   THEN m.title ELSE ''
		       END AS title,
		       CASE fi.item_type
		           WHEN 'episode' THEN e.duration_ms
		           WHEN 'movie'   THEN m.duration_ms ELSE 0
		       END AS duration_ms
		FROM filler_list_item fi
		LEFT JOIN episode e ON fi.item_type = 'episode' AND fi.item_id = e.episode_id
		LEFT JOIN show    s ON e.show_id = s.show_id
		LEFT JOIN movie   m ON fi.item_type = 'movie'   AND fi.item_id = m.movie_id
		WHERE fi.filler_list_id = ? ORDER BY fi.position
	)");
	q.bind(1, filler_list_id);
	while (q.executeStep()) {
		FillerItemRow r;
		r.id          = q.getColumn(0).getInt64();
		r.item_type   = q.getColumn(1).getString();
		r.item_id     = q.getColumn(2).getString();
		r.position    = q.getColumn(3).getInt();
		r.title       = q.getColumn(4).getString();
		r.duration_ms = q.getColumn(5).getInt64();
		d.items.push_back(std::move(r));
	}
	return d;
}
