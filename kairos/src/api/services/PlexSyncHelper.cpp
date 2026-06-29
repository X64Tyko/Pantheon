#include "PlexSyncHelper.h"
#include "../RouteHelpers.h"
#include "../../db/PlaylistRepository.h"
#include "../../db/SourceRepository.h"
#include "../../source/IMediaSource.h"
#include "../../source/SyncManager.h"
#include <nlohmann/json.hpp>

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
	SourceRepository sr(db);
	std::vector<std::pair<std::string, std::string>> items;
	for (const auto& ri : *raw) {
		auto kairos_id = sr.resolveKairosId(source_id, ri.external_id, ri.item_type);
		if (!kairos_id.empty()) items.push_back({ri.item_type, kairos_id});
	}

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

	SourceRepository sr(db);
	std::vector<std::pair<std::string, std::string>> items;
	int total = 0;

	if (src->sourceType() == "plex") {
		auto raw = src->fetchListItems(external_id, list_kind);
		if (!raw) { route::err(res, 502, "failed to fetch items from source"); return; }
		total = static_cast<int>(raw->size());
		for (const auto& ri : *raw) {
			auto kid = sr.resolveKairosId(source_id, ri.external_id, ri.item_type);
			if (!kid.empty()) items.push_back({ri.item_type, kid});
		}
	} else {
		auto raw = (list_kind == "collection")
			? src->browseCollectionItems(external_id)
			: src->browsePlaylistItems(external_id);
		total = static_cast<int>(raw.size());
		for (const auto& ri : raw) {
			auto kid = sr.resolveKairosId(source_id, ri.external_id, ri.item_type);
			if (!kid.empty()) items.push_back({ri.item_type, kid});
		}
	}

	try {
		PlaylistRepository pr(db);
		pr.replaceListItems(list_type, list_id, items);
		pr.upsertPlexLink(list_type, list_id, source_id, external_id, list_kind);
	} catch (const std::exception& e) { route::err(res, 500, e.what()); return; }

	route::ok(res, json{{"synced", (int)items.size()}, {"total", total}}.dump());
}
