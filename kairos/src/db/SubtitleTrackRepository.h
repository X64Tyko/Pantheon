#pragma once
#include "../model/SubtitleTrack.h"
#include <optional>
#include <string>
#include <vector>

class Database;

class SubtitleTrackRepository {
public:
    explicit SubtitleTrackRepository(Database& db);

    // Ordered by (language, title, subtitle_id) so playback-time negative-
    // index assignment (see PlaybackService.cpp / Hephaestus's Router.cpp)
    // stays stable across reloads and track switches. valid=0 rows (see
    // SubtitleValidation.h) are excluded — this is the playback-serving/
    // language-listing path, and a broken sidecar has no business being
    // offered as a track at all. Use listInvalid() for the admin report
    // that surfaces them instead.
    std::vector<SubtitleTrack> get(const std::string& media_type, const std::string& media_id);

    // Every row with valid=0 across the whole library, most-recently-synced
    // first — the admin's Review > Subtitles tab (GET /api/subtitles/broken).
    // No media_type/media_id scoping: this is a library-wide report, not a
    // per-item lookup.
    std::vector<SubtitleTrack> listInvalid();

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

    // Single-row lookup, for the "re-check this file" admin action — sync's
    // own batch replace is the wrong tool for updating one row in place.
    std::optional<SubtitleTrack> getById(const std::string& subtitle_id);

    // Updates just the validity fields on an existing row in place, without
    // touching anything else — the re-check action re-runs
    // validateSubtitleFile() against the file as it exists on disk right
    // now (e.g. after the admin has gone and fixed/replaced it) and writes
    // the fresh result back here, rather than waiting for the next full sync.
    void updateValidity(const std::string& subtitle_id, bool valid, const std::string& invalid_reason);

    // Manual metadata correction from the admin's Review > Subtitles tab
    // (e.g. the filename-parsing heuristic guessed the wrong language code).
    // NOT durable against a source='file' row: the next full sync still
    // unconditionally deletes-and-reinserts every sidecar-derived row for
    // that item from scratch (see syncSubtitleTracksBatch's own comment —
    // there's no `locked` column here the way ChapterRepository has), so an
    // edit only sticks until the file is rescanned. The UI surfaces that
    // caveat rather than pretending this is permanent.
    void update(const std::string& subtitle_id, const std::string& language, bool forced, bool sdh);

    // Removes just the DB row — callers deleting the underlying file too
    // (see SubtitleService's DELETE /api/subtitles/:id, the only caller)
    // must do that themselves first. Row-only removal on its own doesn't
    // stick: if the file is still on disk and still fails validation, the
    // next sync just re-adds it.
    void remove(const std::string& subtitle_id);

private:
    Database& db_;
};
