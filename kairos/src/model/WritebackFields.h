#pragma once
#include <optional>
#include <string>

// Raw bytes for a poster/art writeback, already resolved from whatever form
// Show/Movie.thumb|art was in (CDN URL, source-relative path, or a "local:"
// on-disk file — see ContentService::fetchImageBytes) — IMediaSource
// implementations just upload bytes, they don't resolve paths themselves.
struct WritebackImage
{
	std::string bytes;
	std::string content_type; // e.g. "image/jpeg" — sent as-is as the upload's Content-Type
};

// Metadata pushed back into a source library by IMediaSource::pushMetadata.
// Text fields: empty string means "don't touch this field" (not "clear it").
//
// JellyfinBaseSource sends genres/actors/director/writer/labels/
// audience_rating/countries/imdb_id/tvdb_id/tmdb_id (see its pushMetadata —
// it already fetches-and-replaces the whole item object, so Genres/Tags/
// ProductionLocations are plain array-replaces, CommunityRating is a plain
// scalar, People/Director/Writer are a Type-scoped replace that leaves
// every other credit type untouched, and ProviderIds is merged key-by-key
// so a provider Pantheon doesn't track isn't wiped). collections remains
// unsent there — no user-reported need yet.
//
// PlexSource sends genres/director/writer/collections/labels/
// audience_rating (movies+shows) and countries (movies only, matching
// Plex's own ShowEditMixins not having a Country mixin) via a
// fresh-fetch-then-diff against Plex's add/remove tag directives
// (`tag[n].tag.tag=value` to add/set an index, `tag[].tag.tag-` to remove,
// no single "replace" directive) — verified against python-plexapi's
// actual implementation, not guessed.
//
// `actors` is NOT sent by either source: Jellyfin's is a documented gap (no
// user-reported need yet), but Plex's is structural — even python-plexapi,
// the reference client library, has no Role/Actor edit mixin at all, so
// there's no known-safe mechanism to verify against, let alone implement.
//
// imdb_id/tvdb_id/tmdb_id are NOT sent to Plex — this matters more than a
// typical deferred field: if a scraper match gets corrected in Pantheon but
// the source's own stored external ID is never updated, that source's own
// periodic "refresh from agent" will keep re-pulling the WRONG metadata
// forever, silently undoing the correction. Jellyfin's ProviderIds is a
// plain field on the same whole-object POST everything else already uses,
// so it's safe to send. Plex has no equivalent field edit — its external ID
// is only changeable via a "match" call that re-associates the whole item
// with a new agent result and triggers Plex's own fresh re-scrape of
// everything (poster, summary, etc.), a meaningfully different and more
// invasive operation than a field edit, and not yet researched/verified
// here. Until that's designed properly, a Plex-sourced item with a
// corrected match will NOT get its own re-scrape prevented — worth knowing
// if that's the failure mode being guarded against.
//
// thumb/art: unset optional means "don't touch"; set means "replace with
// these bytes" (images have no partial-clear concept, unlike the text
// fields' empty-string convention).
struct WritebackFields
{
	std::string title;
	std::string original_title;
	std::string overview;
	std::string genres; // JSON array string, same encoding as Show/Movie.genres
	std::string content_rating;
	std::string studio;
	std::string network;     // show-only
	std::string director;    // movie-only
	std::string writer;      // movie-only
	std::string tagline;     // movie-only
	std::string actors;      // JSON array string
	std::string countries;   // JSON array string
	std::string collections; // JSON array string
	std::string labels;      // JSON array string
	std::optional<double> audience_rating;
	std::string imdb_id;
	std::string tvdb_id; // show-only (movies don't carry a tvdb_id — see MovieDetail)
	std::string tmdb_id;
	std::string release_date;            // movie: release_date; show: originally_available_at
	std::optional<WritebackImage> thumb; // poster
	std::optional<WritebackImage> art;   // backdrop/fanart

	// match_confirmed / locked as they stand in Pantheon's own DB right now —
	// only meaningful to LocalSource::pushMetadata, which round-trips them
	// into a movie.nfo/tvshow.nfo's <pantheon_confirmed>/<lockdata> tags so a
	// DB wipe + rescan can restore match_confirmed (not just the match
	// itself) via the trusted-ID short-circuit in matchShow()/matchMovie().
	// Every other IMediaSource ignores these — Plex/Jellyfin have no
	// equivalent "human reviewed this" concept to write into.
	bool match_confirmed = false;
	bool locked          = false;
};