#include "RestrictionRepository.h"
#include "Database.h"
#include "../scraper/RatingSeverity.h"
#include <SQLiteCpp/SQLiteCpp.h>

RestrictionRepository::RestrictionRepository(Database& db) : db_(db) {}

bool RestrictionRepository::isAllowed(const AuthUser& user, const std::string& entity_type,
                                       const std::string& entity_id,
                                       const std::string& content_rating) const {
    if (!user.restricted) return true;

    // Explicit per-title/per-channel override always wins over the ceiling.
    SQLite::Statement q(db_.get(),
        "SELECT mode FROM user_content_override WHERE user_id=? AND entity_type=? AND entity_id=?");
    q.bind(1, user.user_id);
    q.bind(2, entity_type);
    q.bind(3, entity_id);
    if (q.executeStep()) return q.getColumn(0).getString() == "allow";

    if (entity_type == "show") {
        return RatingSeverity::tvRatingSeverity(content_rating)
            <= RatingSeverity::tvRatingSeverity(user.max_tv_rating);
    }
    if (entity_type == "movie") {
        return RatingSeverity::movieRatingSeverity(content_rating)
            <= RatingSeverity::movieRatingSeverity(user.max_movie_rating);
    }
    // Channels always use the TV scale — the admin-facing content_tag and
    // max_channel_rating dropdowns both only offer TV-scale values, so this
    // is never a cross-scale comparison in practice.
    return RatingSeverity::tvRatingSeverity(content_rating)
        <= RatingSeverity::tvRatingSeverity(user.max_channel_rating);
}

std::vector<RestrictionRepository::Override>
RestrictionRepository::listOverrides(const std::string& user_id) const {
    SQLite::Statement q(db_.get(), R"(
        SELECT uco.entity_type, uco.entity_id, uco.mode,
               COALESCE(sh.title, mv.title, ch.name, '')
        FROM user_content_override uco
        LEFT JOIN show    sh ON uco.entity_type='show'    AND sh.show_id=uco.entity_id
        LEFT JOIN movie   mv ON uco.entity_type='movie'   AND mv.movie_id=uco.entity_id
        LEFT JOIN channel ch ON uco.entity_type='channel' AND ch.channel_id=uco.entity_id
        WHERE uco.user_id=?
    )");
    q.bind(1, user_id);

    std::vector<Override> out;
    while (q.executeStep())
        out.push_back({q.getColumn(0).getString(), q.getColumn(1).getString(),
                        q.getColumn(2).getString(), q.getColumn(3).getString()});
    return out;
}

void RestrictionRepository::setOverride(const std::string& user_id, const std::string& entity_type,
                                         const std::string& entity_id, const std::string& mode) {
    SQLite::Statement s(db_.get(), R"(
        INSERT INTO user_content_override (user_id, entity_type, entity_id, mode)
        VALUES (?, ?, ?, ?)
        ON CONFLICT(user_id, entity_type, entity_id) DO UPDATE SET mode = excluded.mode
    )");
    s.bind(1, user_id);
    s.bind(2, entity_type);
    s.bind(3, entity_id);
    s.bind(4, mode);
    s.exec();
}

void RestrictionRepository::removeOverride(const std::string& user_id, const std::string& entity_type,
                                            const std::string& entity_id) {
    SQLite::Statement s(db_.get(),
        "DELETE FROM user_content_override WHERE user_id=? AND entity_type=? AND entity_id=?");
    s.bind(1, user_id);
    s.bind(2, entity_type);
    s.bind(3, entity_id);
    s.exec();
}
