#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// One physical file belonging to a multi-part movie (see Movie::parts).
struct MoviePart
{
	int part_num = 0;
	std::string file_path;
	int64_t duration_ms = 0;
};

struct Movie
{
	std::string movie_id;
	std::string title;
	std::string original_title; // title in the movie's original language, when a source distinguishes it
	std::string content_rating;
	// Single-file movies: the file. Multi-part movies: left as sync last set
	// it (unused for playback) — use `parts` instead, and note duration_ms
	// becomes the SUM of part durations for scheduling. is_multi_part/parts
	// are populated at sync time when a source's fetchMovies() detects
	// sibling files (CD1/CD2, Part 1/Part 2, Disc 1/Disc 2) for one title.
	std::string file_path;
	int64_t duration_ms = 0;
	bool is_multi_part  = false;
	std::vector<MoviePart> parts;
	std::optional<int> year;
	std::string release_date; // "YYYY-MM-DD" when known; empty if the source never provided one

	std::string overview;
	std::string tagline;
	std::string studio;
	std::string director;
	std::string writer;      // first TMDB crew credit with job "Writer" (fallback "Screenplay")
	std::string genres;      // JSON array string
	std::string tags = "[]"; // JSON array: scraper-derived keywords/themes — see Show.h's identical field
	std::string thumb;
	std::string art;
	std::string imdb_id;
	std::string tmdb_id;
	std::optional<float> audience_rating;
	std::string labels;      // JSON array: ["tag", ...]
	std::string actors;      // JSON array of names
	std::string countries;   // JSON array: ["United States", ...]
	std::string collections; // JSON array: ["Marvel", ...]
	// Bucketed ffprobe resolution ("4K"/"1080p"/"720p"/"SD"), probed once at
	// sync time alongside duration validation — see MediaProbe::probeVideoInfo.
	std::string resolution_label;
	// JSON arrays of ISO 639-2 language codes — see Episode.h's identical
	// fields (including why the default is "[]", not "") for the full
	// rationale.
	std::string audio_languages             = "[]";
	std::string embedded_subtitle_languages = "[]";
	// When this item was first added to the library, from the source's own
	// signal (Plex addedAt / Jellyfin DateCreated / Local file mtime as a
	// last resort) — see the library_source_priority merge for what wins
	// when the same item is matched across multiple sources.
	std::optional<int64_t> added_at;
	std::string added_at_source; // "plex" | "jellyfin" | "local" — see Show.h's identical field

	// Watch state as reported by the source for its configured primary
	// account — transient, sync-time-only fields; never written to the
	// `movie` table. Consumed by SyncManager to seed watch_progress for
	// whichever local user the source is mapped to (media_source.synced_user_id).
	std::optional<bool> src_watched;
	std::optional<int64_t> src_position_ms;
	std::optional<int64_t> src_watched_at; // epoch seconds, when the source provides one
	std::optional<int64_t> src_view_count; // real rewatch count when the source reports one (Plex viewCount / Jellyfin PlayCount)

	// See Show.h's identical field — same <pantheon_confirmed> NFO round-trip,
	// read from movie.nfo/"<video>.nfo" instead of tvshow.nfo.
	bool nfo_confirmed = false;
};