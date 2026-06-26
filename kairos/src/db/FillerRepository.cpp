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
