#pragma once
#include <string>
#include <vector>
#include "../auth/AuthStore.h"

class Database;

// Parental controls (see architecture context in RatingSeverity.h and the
// "Adult" content_rating tier in AnidbScraper.cpp). Two-layer model: an
// explicit per-title/per-channel override always wins; otherwise the
// content's rating severity is compared against the user's ceiling for that
// entity type.
class RestrictionRepository {
public:
    explicit RestrictionRepository(Database& db);

    // True if `user` may see/play this item. Unrestricted accounts (the
    // common case) always return true without touching the override table.
    // content_rating is whatever the caller already has on hand (show/movie
    // content_rating, or a channel's content_tag) — this never re-fetches it.
    bool isAllowed(const AuthUser& user, const std::string& entity_type,
                   const std::string& entity_id, const std::string& content_rating) const;

    struct Override {
        std::string entity_type;
        std::string entity_id;
        std::string mode;  // "allow" | "block"
        std::string title; // resolved show/movie/channel title; empty if the item was since deleted
    };
    std::vector<Override> listOverrides(const std::string& user_id) const;

    // Upsert — one row per (user_id, entity_type, entity_id).
    void setOverride(const std::string& user_id, const std::string& entity_type,
                      const std::string& entity_id, const std::string& mode);
    void removeOverride(const std::string& user_id, const std::string& entity_type,
                         const std::string& entity_id);

private:
    Database& db_;
};
