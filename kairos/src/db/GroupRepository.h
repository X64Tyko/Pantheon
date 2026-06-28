#pragma once
#include <string>
#include <unordered_set>
#include <vector>

class Database;

struct EpisodeGroupMemberRow {
    int id = 0, part_num = 0, season = 0, episode = 0;
    std::string episode_id, title;
};

struct EpisodeGroupRow {
    std::string group_id, name, group_type;
    std::vector<EpisodeGroupMemberRow> members;
};

struct EpRow {
    std::string ep_id;
    int season = 0, ep_num = 0;
    std::string title;
};

struct ShowForGrouping {
    std::string show_id, title;
};

class GroupRepository {
public:
    explicit GroupRepository(Database& db);

    std::vector<EpisodeGroupRow>    listGroups(const std::string& show_id);
    std::vector<EpRow>              listEpisodes(const std::string& show_id);
    std::unordered_set<std::string> getConfirmedMemberIds(const std::string& show_id);
    std::vector<ShowForGrouping>    listShows();

private:
    Database& db_;
};
