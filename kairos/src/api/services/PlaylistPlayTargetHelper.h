#pragma once
#include <httplib.h>
#include <string>

class Database;

// GET /api/playlists/:id/resolve-play-target's actual algorithm, pulled out
// of PlaylistService.cpp so it's directly unit-testable the same way
// pushListToSource/syncSourceListItems are (see ListPushHelper.h,
// PlexSyncHelper.h) — the route handler itself just does auth + wiring.
//
// Walks the playlist's items in stored order and writes the first one that
// isn't already fully watched (fresh position-vs-duration check via
// WatchProgressRepository::getStates, not the stored rewatch-count column)
// as {"kind", "id", "position_ms"}. If every item is already complete, loops
// back to the first item at position 0 — the playlist equivalent of
// PlaybackService's show resolve-play-target falling back to episode 1 when
// there's no next episode. Writes JSON "null" if the playlist doesn't exist
// or has no items.
void resolvePlaylistPlayTarget(
	httplib::Response&  res,
	const std::string&  playlist_id,
	const std::string&  user_id,
	Database&           db);
