#include "GroupRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>

GroupRepository::GroupRepository(Database& db) : db_(db) {}

std::vector<EpisodeGroupRow> GroupRepository::listGroups(const std::string& show_id) {
    SQLite::Statement q(db_.get(),
        "SELECT group_id, name, group_type FROM episode_group WHERE show_id=? ORDER BY name");
    q.bind(1, show_id);
    std::vector<EpisodeGroupRow> groups;
    while (q.executeStep()) {
        EpisodeGroupRow g;
        g.group_id   = q.getColumn(0).getString();
        g.name       = q.getColumn(1).getString();
        g.group_type = q.getColumn(2).getString();
        SQLite::Statement mq(db_.get(),
            "SELECT egm.id, egm.episode_id, egm.part_num, e.season, e.episode, e.title "
            "FROM episode_group_member egm JOIN episode e ON e.episode_id=egm.episode_id "
            "WHERE egm.group_id=? ORDER BY egm.part_num");
        mq.bind(1, g.group_id);
        while (mq.executeStep()) {
            g.members.push_back({
                mq.getColumn(0).getInt(),
                mq.getColumn(2).getInt(),
                mq.getColumn(3).getInt(),
                mq.getColumn(4).getInt(),
                mq.getColumn(1).getString(),
                mq.getColumn(5).getString(),
            });
        }
        groups.push_back(std::move(g));
    }
    return groups;
}

std::vector<EpRow> GroupRepository::listEpisodes(const std::string& show_id) {
    SQLite::Statement q(db_.get(),
        "SELECT episode_id, season, episode, title FROM episode "
        "WHERE show_id=? ORDER BY season, episode");
    q.bind(1, show_id);
    std::vector<EpRow> rows;
    while (q.executeStep())
        rows.push_back({q.getColumn(0).getString(), q.getColumn(1).getInt(),
                        q.getColumn(2).getInt(), q.getColumn(3).getString()});
    return rows;
}

std::unordered_set<std::string> GroupRepository::getConfirmedMemberIds(const std::string& show_id) {
    SQLite::Statement q(db_.get(),
        "SELECT egm.episode_id FROM episode_group_member egm "
        "JOIN episode_group eg ON eg.group_id = egm.group_id WHERE eg.show_id = ?");
    q.bind(1, show_id);
    std::unordered_set<std::string> ids;
    while (q.executeStep()) ids.insert(q.getColumn(0).getString());
    return ids;
}

std::vector<ShowForGrouping> GroupRepository::listShows() {
    SQLite::Statement q(db_.get(), "SELECT show_id, title FROM show ORDER BY title");
    std::vector<ShowForGrouping> rows;
    while (q.executeStep())
        rows.push_back({q.getColumn(0).getString(), q.getColumn(1).getString()});
    return rows;
}
