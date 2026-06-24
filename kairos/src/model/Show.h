#pragma once
#include <optional>
#include <string>

struct Show {
    std::string show_id;
    std::string title;
    std::string content_rating;
    std::string overview;
    std::string studio;
    std::string status;           // e.g. "Continuing", "Ended"
    std::string genres;           // JSON array string: ["Drama","Sci-Fi"]
    std::string thumb;            // Plex relative path or custom URL
    std::string art;              // backdrop
    std::string imdb_id;
    std::string tvdb_id;
    std::string tmdb_id;
    std::string originally_available_at;
    std::optional<int>   year;
    std::optional<float> audience_rating;
    std::string labels;       // JSON array: ["tag", ...]
    std::string network;      // e.g. "HBO"
    std::string actors;       // JSON array of names
    std::string countries;    // JSON array: ["United States", ...]
    std::string collections;  // JSON array: ["Marvel", ...]
};
