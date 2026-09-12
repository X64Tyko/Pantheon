#pragma once
#include <optional>
#include <string>
#include <cstdint>

struct Episode {
    std::string episode_id;
    std::string show_id;
    int         season      = 0;
    int         episode     = 0;
    std::string title;
    std::string file_path;
    int64_t     duration_ms = 0;

    std::string      overview;
    std::string      air_date;       // "YYYY-MM-DD"
    std::string      thumb;
    std::string      season_name;    // e.g. "Season 1", "Specials"; empty if unknown
    std::optional<int> absolute_index; // TVDB absolute episode number; null if not available

    std::string      tvdb_id;
    std::string      tmdb_id;
    std::string      imdb_id;

    // Bucketed ffprobe resolution ("4K"/"1080p"/"720p"/"SD"), probed once at
    // sync time alongside duration validation — see MediaProbe::probeVideoInfo.
    // Per-episode (not per-show) since a show's episodes can genuinely span
    // multiple resolutions (early-season DVD rips vs. later 1080p).
    std::string      resolution_label;

    // JSON arrays of ISO 639-2 language codes, probed once at sync time via
    // MediaProbe::probeFileInfo (see SyncManager::syncMediaProbeFromFiles).
    // embedded_subtitle_languages is embedded-stream languages only — the
    // filterable "subtitle_language" field additionally unions in external
    // sidecar-file languages (see SubtitleSidecar.h, subtitle_track table)
    // at query time, not here. Default "[]" (not ""): syncShows() no longer
    // populates these inline, and the upsert's COALESCE(NULLIF(excluded.X,
    // '[]'), X) merge logic depends on an untouched value binding as the
    // JSON-empty-array sentinel, not a bare empty string, or it would
    // clobber the existing DB value with a real empty string every sync.
    std::string      audio_languages = "[]";
    std::string      embedded_subtitle_languages = "[]";

    // Watch state as reported by the source for its configured primary
    // account — transient, sync-time-only fields; never written to the
    // `episode` table. See Movie.h's identical fields for the full rationale.
    std::optional<bool>    src_watched;
    std::optional<int64_t> src_position_ms;
    std::optional<int64_t> src_watched_at;
    std::optional<int64_t> src_view_count; // real rewatch count when the source reports one (Plex viewCount / Jellyfin PlayCount)
};
