#include "ChapterRepository.h"
#include "Database.h"
#include "DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

ChapterRepository::ChapterRepository(Database& db) : db_(db) {}

std::vector<Chapter> ChapterRepository::get(const std::string& media_type,
                                             const std::string& media_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT chapter_id, media_type, media_id, chapter_type, title,
               start_ms, end_ms, position, source, locked
        FROM chapter
        WHERE media_type = ? AND media_id = ?
        ORDER BY position, start_ms
    )");
    q.bind(1, media_type);
    q.bind(2, media_id);

    std::vector<Chapter> result;
    while (q.executeStep()) {
        Chapter c;
        c.chapter_id   = q.getColumn(0).getString();
        c.media_type   = q.getColumn(1).getString();
        c.media_id     = q.getColumn(2).getString();
        c.chapter_type = q.getColumn(3).getString();
        c.title        = q.getColumn(4).getString();
        c.start_ms     = q.getColumn(5).getInt64();
        c.end_ms       = q.getColumn(6).getInt64();
        c.position     = q.getColumn(7).getInt();
        c.source       = q.getColumn(8).getString();
        c.locked       = q.getColumn(9).getInt() != 0;
        result.push_back(std::move(c));
    }
    return result;
}

void ChapterRepository::syncChapters(const std::string& media_type,
                                      const std::string& media_id,
                                      const std::string& source,
                                      std::vector<Chapter> chapters) {
    SQLite::Transaction txn(db_.get());

    { SQLite::Statement d(db_.get(),
          "DELETE FROM chapter WHERE media_type=? AND media_id=? AND source=? AND locked=0");
      d.bind(1, media_type); d.bind(2, media_id); d.bind(3, source); d.exec(); }

    SQLite::Statement ins(db_.get(), R"(
        INSERT INTO chapter (chapter_id, media_type, media_id, chapter_type, title,
                             start_ms, end_ms, position, source, locked)
        VALUES (?,?,?,?,?,?,?,?,?,0)
    )");
    for (auto& c : chapters) {
        if (c.chapter_id.empty()) c.chapter_id = db::generateId();
        ins.bind(1, c.chapter_id);
        ins.bind(2, media_type);
        ins.bind(3, media_id);
        ins.bind(4, c.chapter_type.empty() ? std::string("unclassified") : c.chapter_type);
        ins.bind(5, c.title);
        ins.bind(6, c.start_ms);
        ins.bind(7, c.end_ms);
        ins.bind(8, c.position);
        ins.bind(9, source);
        ins.exec();
        ins.reset();
    }
    txn.commit();
}

std::string ChapterRepository::create(const std::string& media_type,
                                       const std::string& media_id,
                                       const json& j) {
    const std::string id = db::generateId();
    SQLite::Statement s(db_.get(), R"(
        INSERT INTO chapter (chapter_id, media_type, media_id, chapter_type, title,
                             start_ms, end_ms, position, source, locked)
        VALUES (?,?,?,?,?,?,?,?,?,1)
    )");
    s.bind(1, id);
    s.bind(2, media_type);
    s.bind(3, media_id);
    s.bind(4, j.value("chapter_type", std::string("unclassified")));
    s.bind(5, j.value("title",        std::string("")));
    s.bind(6, j.value("start_ms",     int64_t{0}));
    s.bind(7, j.value("end_ms",       int64_t{0}));
    s.bind(8, j.value("position",     int{0}));
    s.bind(9, std::string("manual"));
    s.exec();
    return id;
}

void ChapterRepository::update(const std::string& chapter_id, const json& j) {
    if (j.contains("chapter_type")) {
        SQLite::Statement s(db_.get(),
            "UPDATE chapter SET chapter_type=?, locked=1 WHERE chapter_id=?");
        s.bind(1, j["chapter_type"].get<std::string>()); s.bind(2, chapter_id); s.exec();
    }
    if (j.contains("title")) {
        SQLite::Statement s(db_.get(),
            "UPDATE chapter SET title=?, locked=1 WHERE chapter_id=?");
        s.bind(1, j["title"].get<std::string>()); s.bind(2, chapter_id); s.exec();
    }
    if (j.contains("start_ms")) {
        SQLite::Statement s(db_.get(),
            "UPDATE chapter SET start_ms=?, locked=1 WHERE chapter_id=?");
        s.bind(1, j["start_ms"].get<int64_t>()); s.bind(2, chapter_id); s.exec();
    }
    if (j.contains("end_ms")) {
        SQLite::Statement s(db_.get(),
            "UPDATE chapter SET end_ms=?, locked=1 WHERE chapter_id=?");
        s.bind(1, j["end_ms"].get<int64_t>()); s.bind(2, chapter_id); s.exec();
    }
    if (j.contains("position")) {
        SQLite::Statement s(db_.get(),
            "UPDATE chapter SET position=?, locked=1 WHERE chapter_id=?");
        s.bind(1, j["position"].get<int>()); s.bind(2, chapter_id); s.exec();
    }
}

void ChapterRepository::remove(const std::string& chapter_id) {
    SQLite::Statement s(db_.get(), "DELETE FROM chapter WHERE chapter_id=?");
    s.bind(1, chapter_id);
    s.exec();
}

json ChapterRepository::toJson(const Chapter& c) {
    return {
        {"chapter_id",   c.chapter_id},
        {"media_type",   c.media_type},
        {"media_id",     c.media_id},
        {"chapter_type", c.chapter_type},
        {"title",        c.title},
        {"start_ms",     c.start_ms},
        {"end_ms",       c.end_ms},
        {"position",     c.position},
        {"source",       c.source},
        {"locked",       c.locked}
    };
}

json ChapterRepository::toJson(const std::vector<Chapter>& chapters) {
    json arr = json::array();
    for (const auto& c : chapters) arr.push_back(toJson(c));
    return arr;
}
