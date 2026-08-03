#pragma once
#include "IMediaSource.h"
#include <string>

class ConfStore;

// Local filesystem source. base_path_ is the root directory of the library.
//
// Expected layout:
//   Shows:  {base}/{Show Name}/{Season N}/{S01E01 - Title.ext}
//   Movies: {base}/{Title (Year)}/{movie.ext}  or  {base}/{Title (Year).ext}
//
// Shows and movies can coexist under the same base_path_ ("mixed" library type).
// The library type configured in media_library controls which fetchX() methods
// SyncManager calls — this source supports all three layouts.
class LocalSource final : public IMediaSource
{
public:
	LocalSource(const std::string& source_id, const std::string& base_path, ConfStore& conf);

	std::string sourceId() const override { return source_id_; }
	std::string sourceType() const override { return "local"; }
	bool isSupported() const override { return true; }

	// Returns one library entry per immediate subdirectory of base_path_.
	std::vector<LibraryInfo> listAvailableLibraries() override;

	// Returns subdirectories at path (must be within base_path_).
	// Used by the /api/sources/:id/fs endpoint for the folder browser.
	std::vector<LibraryInfo> listSubdirectories(const std::string& path);

	// Each top-level subdirectory is treated as a show — unless library_type
	// is "mixed", in which case only subdirectories that structurally look
	// like a show (season subdirectories, or episode-numbered filenames) are
	// included, so a mixed library doesn't also create a fake show for every
	// movie folder.
	std::vector<Show> fetchShows(const std::string& external_lib_id) override;
	// Each top-level subdirectory (or bare video file) is treated as a movie —
	// same "mixed" filtering as fetchShows, in reverse.
	std::vector<Movie> fetchMovies(const std::string& external_lib_id) override;
	// external_show_id is the show directory path returned by fetchShows.
	std::vector<Episode> fetchEpisodes(const std::string& external_show_id) override;

	// Writes corrected metadata into a movie.nfo/tvshow.nfo (+ poster/fanart)
	// sidecar next to the item's own files — external_id is the item's own
	// (possibly path-mapped) movie_id/show_id, same as everywhere else in
	// IMediaSource. external_lib_id is unused (local items don't need a
	// library-scoped write endpoint). item_type must be "movie" or "show" —
	// there is no per-episode writeback caller today.
	bool pushMetadata(const std::string& external_id,
					  const std::string& external_lib_id,
					  const std::string& item_type,
					  const WritebackFields& fields) override;

	std::vector<Playlist> fetchPlaylists(const std::string&) override { return {}; }
	std::vector<BrowseListItem> browsePlaylists() override { return {}; }
	std::vector<BrowseContentItem> browsePlaylistItems(const std::string&) override { return {}; }
	std::vector<BrowseListItem> browseCollections(const std::string&) override { return {}; }
	std::vector<BrowseContentItem> browseCollectionItems(const std::string&) override { return {}; }

private:
	std::string source_id_;
	std::string base_path_;
	ConfStore& conf_;
};