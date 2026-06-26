#include "PlexSyncHelper.h"
#include "../RouteHelpers.h"
#include "../../db/Database.h"
#include "../../source/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <ctime>

using json = nlohmann::json;

void syncPlexListItems(
	httplib::Response&  res,
	const std::string&  list_type,
	const std::string&  list_id,
	const std::string&  source_id,
	const std::string&  external_id,
	const std::string&  plex_type,
	Database&           db,
	SyncManager&        sync)
{
	auto src = sync.findSource(source_id);
	if (!src) { route::err(res, 404, "source not found"); return; }
	if (src->sourceType() != "plex") { route::err(res, 400, "source is not a Plex source"); return; }

	auto raw = src->fetchListItems(external_id, plex_type);
	if (!raw) { route::err(res, 502, "failed to fetch items from Plex"); return; }

	int total = static_cast<int>(raw->size());
	struct Item { std::string item_type; std::string kairos_id; };
	std::vector<Item> items;
	for (const auto& ri : *raw) {
		SQLite::Statement lk(db.get(),
			"SELECT kairos_id FROM source_mapping WHERE source_id=? AND external_id=? AND item_type=?");
		lk.bind(1, source_id); lk.bind(2, ri.external_id); lk.bind(3, ri.item_type);
		if (lk.executeStep()) items.push_back({ri.item_type, lk.getColumn(0).getString()});
	}

	const std::string fk_col   = (list_type == "playlist") ? "playlist_id"   : "filler_list_id";
	const std::string item_tbl = (list_type == "playlist") ? "playlist_item" : "filler_list_item";
	try {
		SQLite::Transaction txn(db.get());

		SQLite::Statement del(db.get(),
			"DELETE FROM " + item_tbl + " WHERE " + fk_col + " = ?");
		del.bind(1, list_id); del.exec();

		int pos = 0;
		for (const auto& item : items) {
			SQLite::Statement ins(db.get(),
				"INSERT OR IGNORE INTO " + item_tbl +
				" (" + fk_col + ", position, item_type, item_id) VALUES (?,?,?,?)");
			ins.bind(1, list_id); ins.bind(2, pos++);
			ins.bind(3, item.item_type); ins.bind(4, item.kairos_id);
			ins.exec();
		}

		int64_t now = static_cast<int64_t>(std::time(nullptr));
		SQLite::Statement ul(db.get(), R"(
			INSERT INTO plex_list_link (list_type, list_id, source_id, external_id, plex_type, last_synced_at)
			VALUES (?,?,?,?,?,?)
			ON CONFLICT(list_type, list_id) DO UPDATE SET
				source_id      = excluded.source_id,
				external_id    = excluded.external_id,
				plex_type      = excluded.plex_type,
				last_synced_at = excluded.last_synced_at
		)");
		ul.bind(1, list_type); ul.bind(2, list_id); ul.bind(3, source_id);
		ul.bind(4, external_id); ul.bind(5, plex_type); ul.bind(6, now);
		ul.exec();

		txn.commit();
	} catch (const std::exception& e) { route::err(res, 500, e.what()); return; }

	route::ok(res, json{{"synced", (int)items.size()}, {"total", total}}.dump());
}
