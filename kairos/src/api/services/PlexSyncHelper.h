#pragma once
#include <httplib.h>
#include <string>

class Database;
class SyncManager;

// Plex-only legacy helper (kept for backwards compat).
// list_type: "playlist" | "filler_list"
void syncPlexListItems(
	httplib::Response&  res,
	const std::string&  list_type,
	const std::string&  list_id,
	const std::string&  source_id,
	const std::string&  external_id,
	const std::string&  plex_type,
	Database&           db,
	SyncManager&        sync);

// Generic helper for any source type (Plex, Jellyfin, Emby).
// Uses browsePlaylistItems / browseCollectionItems via the source interface.
// list_kind: "playlist" | "collection"
void syncSourceListItems(
	httplib::Response&  res,
	const std::string&  list_type,
	const std::string&  list_id,
	const std::string&  source_id,
	const std::string&  external_id,
	const std::string&  list_kind,
	Database&           db,
	SyncManager&        sync);
