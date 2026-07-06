#pragma once
#include "../model/Chapter.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class Database;

class ChapterRepository {
public:
    explicit ChapterRepository(Database& db);

    std::vector<Chapter> get(const std::string& media_type, const std::string& media_id);

    // Replace all unlocked chapters for (media_type, media_id, source) with new_chapters.
    // Locked rows (manually edited) are never touched.
    void syncChapters(const std::string& media_type,
                      const std::string& media_id,
                      const std::string& source,
                      std::vector<Chapter> chapters);

    // Manual CRUD — create/update always set locked=1.
    std::string create(const std::string& media_type,
                       const std::string& media_id,
                       const nlohmann::json& j);
    void update(const std::string& chapter_id, const nlohmann::json& j);
    void remove(const std::string& chapter_id);

    static nlohmann::json toJson(const Chapter& c);
    static nlohmann::json toJson(const std::vector<Chapter>& chapters);

    // ── Review browsing (visual inspection only, no writes) ───────────────────
    struct ReviewItem {
        std::string media_type;  // "episode" | "movie"
        std::string media_id;
        std::string title;       // display title — "Show — SxxEyy Title" for episodes
        std::string thumb;
        std::string file_path;
        int64_t     duration_ms = 0;
        int         year        = 0;
        std::vector<Chapter> chapters;
    };
    struct ReviewResult { std::vector<ReviewItem> items; int total = 0; };

    // media_type_filter/chapter_type_filter: "all" or a specific value to match.
    ReviewResult listReviewItems(const std::string& media_type_filter,
                                  const std::string& chapter_type_filter,
                                  const std::string& q,
                                  int limit, int offset);

private:
    Database& db_;
};
