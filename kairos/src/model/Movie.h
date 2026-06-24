#pragma once
#include <cstdint>
#include <optional>
#include <string>

struct Movie {
    std::string    movie_id;
    std::string    title;
    std::string    content_rating;
    std::string    file_path;
    int64_t        duration_ms = 0;
    std::optional<int> year;

    std::string    overview;
    std::string    tagline;
    std::string    studio;
    std::string    director;
    std::string    genres;          // JSON array string
    std::string    thumb;
    std::string    art;
    std::string    imdb_id;
    std::string    tmdb_id;
    std::optional<float> audience_rating;
    std::string    labels;       // JSON array: ["tag", ...]
    std::string    actors;       // JSON array of names
    std::string    countries;    // JSON array: ["United States", ...]
    std::string    collections;  // JSON array: ["Marvel", ...]
};
