#include "PlexSyncHelper.h"
#include "../RouteHelpers.h"
#include "../../db/Database.h"
#include "../../db/PlaylistRepository.h"
#include "../../db/SourceRepository.h"
#include "../../source/IMediaSource.h"
#include "../../source/SyncManager.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {
// A collection can contain whole shows, not just movies/episodes (a common
// Plex/Jellyfin use case — e.g. grouping an entire series), but playlist_item
// only supports 'episode'/'movie' (see Database.cpp's CHECK) — so a resolved
// show expands to all its episodes (season/episode order, specials excluded,
// same default as PlaylistRepository::refreshSmart's identical show-type
// expansion) rather than being added directly, which the schema forbids.
// Returns false when the item has never been synced from this source at
// all (nothing to resolve it against) — the caller reports these by title
// rather than silently dropping them, so an import of e.g. a hand-curated
// multi-source watch order (a "Gundam Unicorn order" style collection mixing
// movies/OVAs/specials) surfaces exactly which entries the library is still
// missing instead of a bare, unexplained item-count shortfall.
bool resolveAndExpand(Database& db, SourceRepository& sr, const std::string& source_id,
                      const std::string& item_type, const std::string& external_id,
                      std::vector<std::pair<std::string, std::string>>& out) {
	auto kairos_id = sr.resolveKairosId(source_id, external_id, item_type);
	if (kairos_id.empty()) return false;
	if (item_type != "show") { out.push_back({item_type, kairos_id}); return true; }
	SQLite::Statement eq(db.get(),
		"SELECT episode_id FROM episode WHERE show_id = ? AND season > 0 ORDER BY season, episode");
	eq.bind(1, kairos_id);
	while (eq.executeStep()) out.push_back({"episode", eq.getColumn(0).getString()});
	return true;
}
} // namespace

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
	SourceRepository sr(db);
	std::vector<std::pair<std::string, std::string>> items;
	for (const auto& ri : *raw)
		resolveAndExpand(db, sr, source_id, ri.item_type, ri.external_id, items);

	try {
		PlaylistRepository pr(db);
		pr.replaceListItems(list_type, list_id, items);
		pr.upsertPlexLink(list_type, list_id, source_id, external_id, plex_type);
	} catch (const std::exception& e) { route::err(res, 500, e.what()); return; }

	route::ok(res, json{{"synced", (int)items.size()}, {"total", total}}.dump());
}

void syncSourceListItems(
	httplib::Response&  res,
	const std::string&  list_type,
	const std::string&  list_id,
	const std::string&  source_id,
	const std::string&  external_id,
	const std::string&  list_kind,
	Database&           db,
	SyncManager&        sync)
{
	auto src = sync.findSource(source_id);
	if (!src) { route::err(res, 404, "source not found"); return; }

	// browseCollectionItems/browsePlaylistItems work identically well for
	// Plex, Jellyfin, and Emby alike (unlike fetchListItems, a Plex-only
	// legacy path that also drops title/season/episode info this function
	// needs to report unresolved items by name below) — one code path for
	// every source type instead of a special-cased branch that's no longer
	// actually necessary.
	SourceRepository sr(db);
	auto raw = (list_kind == "collection")
		? src->browseCollectionItems(external_id)
		: src->browsePlaylistItems(external_id);
	int total = static_cast<int>(raw.size());

	std::vector<std::pair<std::string, std::string>> items;
	json unresolved = json::array();
	for (const auto& ri : raw) {
		if (!resolveAndExpand(db, sr, source_id, ri.item_type, ri.external_id, items))
			unresolved.push_back({{"title", ri.title}, {"item_type", ri.item_type}});
	}

	try {
		PlaylistRepository pr(db);
		pr.replaceListItems(list_type, list_id, items);
		pr.upsertPlexLink(list_type, list_id, source_id, external_id, list_kind);
	} catch (const std::exception& e) { route::err(res, 500, e.what()); return; }

	route::ok(res, json{{"synced", (int)items.size()}, {"total", total}, {"unresolved", unresolved}}.dump());
}
