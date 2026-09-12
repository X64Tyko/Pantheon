#include "ListPushHelper.h"
#include "../RouteHelpers.h"
#include "../../db/PlaylistRepository.h"
#include "../../db/SourceRepository.h"
#include "../../source/IMediaSource.h"
#include "../../source/SyncManager.h"
#include <nlohmann/json.hpp>
#include <set>

using json = nlohmann::json;

void pushListToSource(
	httplib::Response&  res,
	const std::string&  list_id,
	const std::string&  source_id,
	const std::string&  list_kind,
	const std::string&  title,
	const std::string&  external_lib_id,
	Database&           db,
	SyncManager&        sync)
{
	auto src = sync.findSource(source_id);
	if (!src) { route::err(res, 404, "source not found"); return; }

	PlaylistRepository pr(db);
	auto detail = pr.getDetail(list_id);
	if (!detail) { route::err(res, 404, "playlist not found"); return; }

	// Every item must already exist in the target source's library — pushed
	// as its resolved external_id there, not created fresh (see
	// IMediaSource::createRemoteList's doc comment). Items never mapped from
	// that source (e.g. only ever synced from a different one) are silently
	// skipped and counted, not treated as a hard failure — a partial push is
	// still useful, same "degrade gracefully" reasoning as elsewhere in sync.
	SourceRepository sr(db);
	std::vector<IMediaSource::PushListItem> resolved;
	int unresolved = 0;
	for (const auto& item : detail->items) {
		auto ext = sr.resolveExternalId(source_id, item.item_id, item.item_type);
		if (ext.empty()) { ++unresolved; continue; }
		resolved.push_back({item.item_type, ext});
	}
	if (resolved.empty()) {
		route::err(res, 400, "none of this playlist's items are mapped to that source — sync it first");
		return;
	}

	auto existing = pr.getLink("playlist", list_id);
	const bool already_linked_here = existing && existing->source_id == source_id && existing->plex_type == list_kind;

	if (!already_linked_here) {
		auto new_id = src->createRemoteList(title.empty() ? detail->title : title, list_kind, resolved, external_lib_id);
		if (!new_id) { route::err(res, 502, "failed to create remote list — see server logs"); return; }
		pr.upsertPlexLink("playlist", list_id, source_id, *new_id, list_kind);
		route::ok(res, json{
			{"created", true}, {"external_id", *new_id},
			{"pushed", (int)resolved.size()}, {"unresolved", unresolved},
		}.dump());
		return;
	}

	// Already linked to this exact (source, kind) — reconcile rather than
	// re-create: diff the remote's current items against what the local
	// playlist has now.
	auto remote_items = (list_kind == "collection") ? src->browseCollectionItems(existing->external_id)
	                                                 : src->browsePlaylistItems(existing->external_id);
	std::set<std::string> remote_ids, local_ids;
	for (const auto& r : remote_items) remote_ids.insert(r.external_id);
	for (const auto& l : resolved)     local_ids.insert(l.external_id);

	std::vector<IMediaSource::PushListItem> to_add, to_remove;
	for (const auto& l : resolved)
		if (!remote_ids.count(l.external_id)) to_add.push_back(l);
	for (const auto& r : remote_items)
		if (!local_ids.count(r.external_id)) to_remove.push_back({r.item_type, r.external_id});

	bool ok = true;
	if (!to_add.empty())    ok = src->addRemoteListItems(existing->external_id, list_kind, to_add) && ok;
	if (!to_remove.empty()) ok = src->removeRemoteListItems(existing->external_id, list_kind, to_remove) && ok;

	if (!ok) { route::err(res, 502, "push partially failed — see server logs"); return; }

	route::ok(res, json{
		{"created", false}, {"external_id", existing->external_id},
		{"added", (int)to_add.size()}, {"removed", (int)to_remove.size()}, {"unresolved", unresolved},
	}.dump());
}
