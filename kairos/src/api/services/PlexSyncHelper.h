#pragma once
#include <httplib.h>
#include <string>

class Database;
class SyncManager;

// Shared helper: fetch items from a Plex playlist/collection via IMediaSource,
// replace list items in the DB, and upsert the plex_list_link record.
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
