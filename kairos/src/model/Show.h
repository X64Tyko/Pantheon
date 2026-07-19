#pragma once
#include <optional>
#include <string>

struct Rating
{
	std::string image;
	std::string type;
	std::optional<int> value;
};

struct ScraperInfo
{
	std::string title;
	std::string scraper_id;
};

struct Show {
    std::string show_id;
    std::string title;
    std::string original_title; // title in the show's original language, when a source distinguishes it
    std::string content_rating;
    std::string overview;
	std::string tagline;
    std::string studio;
    std::string status;           // e.g. "Continuing", "Ended"
    std::string genres;           // JSON array string: ["Drama","Sci-Fi"]
    std::string thumb;            // Plex relative path or custom URL
    std::string art;              // backdrop
    std::string imdb_id;
    std::string tvdb_id;
    std::string tmdb_id;
	std::vector<ScraperInfo> scraper_info; // prep to move beyond a hardcoded 3 scrapers
    std::string originally_available_at;
	std::optional<int64_t> added_at;
	// Which source populated `added_at` on this sync pass ("plex"/"jellyfin"/
	// "local") — lets the upsert compare against library_source_priority
	// instead of blindly taking whichever source happened to sync last.
	std::string added_at_source;
    std::optional<int>   year;
    std::optional<float> audience_rating;
	std::vector<Rating> ratings;
    std::string labels;       // JSON array: ["tag", ...]
    std::string network;      // e.g. "HBO"
    std::string actors;       // JSON array of names
    std::string countries;    // JSON array: ["United States", ...]
    std::string collections;  // JSON array: ["Marvel", ...]
    // On-disk root folder for this show, as reported by the source — raw,
    // unmapped (ConfStore::applyPathMap() is applied at comparison/dedup
    // time, not persisted). Empty if the source doesn't report one.
    std::string folder_path;
};
