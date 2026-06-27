#pragma once
#include <cstdint>
#include <string>

struct Chapter {
    std::string chapter_id;
    std::string media_type;    // "episode" | "movie"
    std::string media_id;
    std::string chapter_type;  // "intro" | "ad_break" | "chapter" | "credits" | "outro" | "unclassified"
    std::string title;
    int64_t     start_ms  = 0;
    int64_t     end_ms    = 0;
    int         position  = 0;
    std::string source;        // "manual" | "plex_intro" | "plex_chapters" | "file"
    bool        locked    = false;
};
