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
};
