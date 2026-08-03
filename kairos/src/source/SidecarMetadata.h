#pragma once
#include <filesystem>
#include <optional>
#include <string>

// Kodi-convention sidecar metadata for LocalSource: .nfo XML files and local
// poster/fanart images sitting next to media on disk. Every string field is
// empty when the .nfo doesn't provide it — callers fill gaps the same way
// LocalSource already treats a blank folder-name-parsed field, they don't
// need to special-case "missing" vs "empty".
//
// thumb/art below are already resolved to a "local:<absolute path>" sentinel
// (see ContentService::isLocalSentinel/proxyImage) — ready to assign straight
// to Show::thumb/Movie::thumb/etc., not a raw filesystem path.

struct NfoMovie
{
	std::string title;
	std::optional<int> year;
	std::string overview;
	std::string tagline;
	std::string content_rating; // <mpaa>
	std::string genres;         // JSON array string
	std::string studio;
	std::string director;
	std::string actors;       // JSON array string
	std::string countries;    // JSON array string
	std::string release_date; // "YYYY-MM-DD"
	std::string imdb_id;
	std::string tmdb_id;
	std::optional<float> audience_rating;
	std::string thumb; // "local:..." or empty
	std::string art;   // "local:..." or empty
	// <pantheon_confirmed> — Pantheon-specific extension tag (other Kodi/Jellyfin/
	// Plex tools just ignore unrecognised elements): set when a human confirmed
	// this match in Pantheon. Read back by LocalSource so a DB wipe + rescan can
	// restore match_confirmed, not just match_status — see matchMovie()'s
	// trusted-ID branch, which is the only place this actually gets applied
	// (never on an already-'matched' row, so an explicit later un-confirm in the
	// DB isn't silently re-applied by the next resync while this tag still sits
	// on disk).
	bool confirmed = false;
	// Kodi's own <lockdata> — protects these fields from scraper/agent overwrite.
	// Mirrors Pantheon's own `locked` column (set whenever a human manually
	// edits a field) — writing it out lets other Kodi-convention tools pointed
	// at the same files respect the same "don't touch this" signal.
	bool locked = false;
};

struct NfoShow
{
	std::string title;
	std::optional<int> year;
	std::string overview;
	std::string content_rating;
	std::string genres;
	std::string network; // <studio>, same convention JellyfinBaseSource reads it under
	std::string status;
	std::string actors;
	std::string countries;
	std::string release_date; // originally_available_at
	std::string imdb_id;
	std::string tmdb_id;
	std::string tvdb_id;
	std::optional<float> audience_rating;
	std::string thumb;
	std::string art;
	bool confirmed = false; // see NfoMovie::confirmed
	bool locked    = false; // see NfoMovie::locked
};

struct NfoEpisode
{
	std::string title;
	std::string overview;
	std::string air_date; // "YYYY-MM-DD"
	std::string thumb;
};

// Looks for <video_path> with its extension swapped for .nfo, then (only
// when has_own_folder is true, i.e. video_path sits in a folder that belongs
// to this movie alone) a fixed "movie.nfo" in that same folder — nullopt if
// neither is present or parsing fails. art_dir is where local poster/fanart
// images are looked for (the movie's own folder, or the library root for a
// bare-file movie), matched case-insensitively against the usual Kodi names
// (poster/folder, fanart/backdrop) plus, for a bare-file movie, "<video
// stem>-poster"/"<video stem>-fanart"/a bare "<video stem>" image.
std::optional<NfoMovie> loadMovieSidecar(const std::filesystem::path& video_path,
										 const std::filesystem::path& art_dir,
										 bool has_own_folder);

// Parses <show_dir>/tvshow.nfo, plus local poster/fanart in the same folder.
std::optional<NfoShow> loadShowSidecar(const std::filesystem::path& show_dir);

// Parses a sidecar .nfo named identically to video_path (extension swapped
// for .nfo) plus a "<stem>-thumb.<ext>" episode-thumbnail image beside it.
std::optional<NfoEpisode> loadEpisodeSidecar(const std::filesystem::path& video_path);

// Raw bytes for a poster/fanart image to write to disk — mirrors
// WritebackImage (model/WritebackFields.h) but this file doesn't depend on
// that header, so it's its own small struct.
struct SidecarImage
{
	std::string bytes;
	std::string content_type; // e.g. "image/jpeg" — determines the written file's extension
};

// Writes/overwrites a Kodi-style NFO for a movie — "<video>.nfo" next to a
// bare-file movie, or "movie.nfo" in a movie's own folder — the same path
// rule loadMovieSidecar() reads from. thumb/art, when set, replace whatever
// poster/fanart image currently occupies that slot (any existing recognised
// name/extension in art_dir is removed first, so re-writing to a different
// content-type doesn't leave a stale image behind alongside the fresh one).
// art_dir/has_own_folder follow the same convention as loadMovieSidecar().
void saveMovieSidecar(const std::filesystem::path& video_path,
					  const std::filesystem::path& art_dir,
					  bool has_own_folder,
					  const NfoMovie& data,
					  const std::optional<SidecarImage>& thumb,
					  const std::optional<SidecarImage>& art);

// Writes/overwrites <show_dir>/tvshow.nfo, plus poster/fanart in the same
// folder (same replace-stale-image behaviour as saveMovieSidecar()).
void saveShowSidecar(const std::filesystem::path& show_dir,
					 const NfoShow& data,
					 const std::optional<SidecarImage>& thumb,
					 const std::optional<SidecarImage>& art);