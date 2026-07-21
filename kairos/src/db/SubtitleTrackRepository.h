#pragma once
#include "../model/SubtitleTrack.h"
#include <string>
#include <vector>

class Database;

class SubtitleTrackRepository {
public:
    explicit SubtitleTrackRepository(Database& db);

    // Ordered by (language, title, subtitle_id) so playback-time negative-
    // index assignment (see PlaybackService.cpp / Hephaestus's Router.cpp)
    // stays stable across reloads and track switches.
    std::vector<SubtitleTrack> get(const std::string& media_type, const std::string& media_id);

    // Replace all source-matching rows for (media_type, media_id) with
    // new_tracks — mirrors ChapterRepository::syncChapters's delete-then-
    // insert. Unlike chapters, there's no locked flag to preserve: no
    // manual-edit UI exists for subtitle_track rows yet.
    void syncSubtitleTracks(const std::string& media_type,
                             const std::string& media_id,
                             const std::string& source,
                             std::vector<SubtitleTrack> tracks);

    // Same replace-semantics as syncSubtitleTracks, batched across every id
    // in media_ids in a single transaction — one bulk DELETE (via json_each)
    // plus one bulk INSERT instead of one transaction per id. Used by
    // SyncManager's post-pass so a full library scan doesn't open a write
    // transaction per file. `tracks` holds only the ids that currently have
    // sidecar tracks; every id in media_ids is still cleared first, so ids
    // with none simply end up with no rows (same as calling
    // syncSubtitleTracks(..., {}) for them individually).
    void syncSubtitleTracksBatch(const std::string& media_type,
                                  const std::vector<std::string>& media_ids,
                                  const std::string& source,
                                  std::vector<SubtitleTrack> tracks);

private:
    Database& db_;
};
