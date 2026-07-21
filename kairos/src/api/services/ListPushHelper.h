#pragma once
#include <httplib.h>
#include <string>

class Database;
class SyncManager;

// Pushes a local playlist's items TO a remote Plex/Jellyfin/Emby playlist or
// collection — the write direction, symmetric with syncSourceListItems's
// pull (PlexSyncHelper.h). If this playlist has never been linked
// (PlaylistRepository::getLink) — or is linked to a *different* source/kind —
// creates a brand-new remote list and (re-)links to it; if already linked to
// this exact (source_id, list_kind), reconciles instead of re-creating: adds
// items present locally but missing remotely, removes items present
// remotely but no longer in the local playlist.
//
// Only wired for playlists in this first pass, not filler lists — the
// underlying plex_list_link table already generalizes across list_type
// (see PlaylistRepository::upsertPlexLink), so extending this to filler
// lists later is just parameterizing list_type, not new plumbing.
//
// list_kind: "playlist" | "collection". external_lib_id is only meaningful
// for kind="collection" (Plex collections are scoped to a library section) —
// see IMediaSource::createRemoteList.
void pushListToSource(
	httplib::Response&  res,
	const std::string&  list_id,
	const std::string&  source_id,
	const std::string&  list_kind,
	const std::string&  title,
	const std::string&  external_lib_id,
	Database&           db,
	SyncManager&        sync);
