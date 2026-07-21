#include "SubtitleTrackRepository.h"
#include "Database.h"
#include "DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

SubtitleTrackRepository::SubtitleTrackRepository(Database& db) : db_(db) {}

std::vector<SubtitleTrack> SubtitleTrackRepository::get(const std::string& media_type,
                                                          const std::string& media_id) {
    SQLite::Statement q(db_.get(), R"(
        SELECT subtitle_id, media_type, media_id, file_path, language, forced, sdh, title, source
        FROM subtitle_track
        WHERE media_type = ? AND media_id = ?
        ORDER BY language, title, subtitle_id
    )");
    q.bind(1, media_type);
    q.bind(2, media_id);

    std::vector<SubtitleTrack> result;
    while (q.executeStep()) {
        SubtitleTrack t;
        t.subtitle_id = q.getColumn(0).getString();
        t.media_type  = q.getColumn(1).getString();
        t.media_id    = q.getColumn(2).getString();
        t.file_path   = q.getColumn(3).getString();
        t.language    = q.getColumn(4).getString();
        t.forced      = q.getColumn(5).getInt() != 0;
        t.sdh         = q.getColumn(6).getInt() != 0;
        t.title       = q.getColumn(7).getString();
        t.source      = q.getColumn(8).getString();
        result.push_back(std::move(t));
    }
    return result;
}

void SubtitleTrackRepository::syncSubtitleTracks(const std::string& media_type,
                                                  const std::string& media_id,
                                                  const std::string& source,
                                                  std::vector<SubtitleTrack> tracks) {
    SQLite::Transaction txn(db_.get());

    { SQLite::Statement d(db_.get(),
          "DELETE FROM subtitle_track WHERE media_type=? AND media_id=? AND source=?");
      d.bind(1, media_type); d.bind(2, media_id); d.bind(3, source); d.exec(); }

    SQLite::Statement ins(db_.get(), R"(
        INSERT INTO subtitle_track (subtitle_id, media_type, media_id, file_path,
                                     language, forced, sdh, title, source)
        VALUES (?,?,?,?,?,?,?,?,?)
    )");
    for (auto& t : tracks) {
        if (t.subtitle_id.empty()) t.subtitle_id = db::generateId();
        ins.bind(1, t.subtitle_id);
        ins.bind(2, media_type);
        ins.bind(3, media_id);
        ins.bind(4, t.file_path);
        ins.bind(5, t.language);
        ins.bind(6, t.forced ? 1 : 0);
        ins.bind(7, t.sdh ? 1 : 0);
        ins.bind(8, t.title);
        ins.bind(9, source);
        ins.exec();
        ins.reset();
    }
    txn.commit();
}

void SubtitleTrackRepository::syncSubtitleTracksBatch(const std::string& media_type,
                                                        const std::vector<std::string>& media_ids,
                                                        const std::string& source,
                                                        std::vector<SubtitleTrack> tracks) {
    if (media_ids.empty()) return;

    SQLite::Transaction txn(db_.get());

    { SQLite::Statement d(db_.get(), R"(
          DELETE FROM subtitle_track
          WHERE media_type = ? AND source = ?
            AND media_id IN (SELECT value FROM json_each(?))
      )");
      d.bind(1, media_type);
      d.bind(2, source);
      d.bind(3, nlohmann::json(media_ids).dump());
      d.exec(); }

    SQLite::Statement ins(db_.get(), R"(
        INSERT INTO subtitle_track (subtitle_id, media_type, media_id, file_path,
                                     language, forced, sdh, title, source)
        VALUES (?,?,?,?,?,?,?,?,?)
    )");
    for (auto& t : tracks) {
        if (t.subtitle_id.empty()) t.subtitle_id = db::generateId();
        ins.bind(1, t.subtitle_id);
        ins.bind(2, media_type);
        ins.bind(3, t.media_id);
        ins.bind(4, t.file_path);
        ins.bind(5, t.language);
        ins.bind(6, t.forced ? 1 : 0);
        ins.bind(7, t.sdh ? 1 : 0);
        ins.bind(8, t.title);
        ins.bind(9, source);
        ins.exec();
        ins.reset();
    }
    txn.commit();
}
