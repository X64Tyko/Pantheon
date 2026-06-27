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

private:
    Database& db_;
};
