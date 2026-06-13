#pragma once
#include <string>
#include <vector>

struct PlaylistItem {
    std::string playlist_id;
    int         position  = 0;
    std::string item_type; // "episode" | "movie"
    std::string item_id;
};

struct Playlist {
    std::string             playlist_id;
    std::string             title;
    std::string             source; // "plex" | "jellyfin" | "emby" | "custom"
    std::vector<PlaylistItem> items; // populated on load
};
