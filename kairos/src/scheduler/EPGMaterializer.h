#pragma once
#include <ctime>
#include <string>
#include "RuleEngine.h"

class Database;

class EPGMaterializer {
public:
    EPGMaterializer(Database& db, RuleEngine& engine);

    // Ensure the schedule for `channel_id` is populated from `from` through
    // `from + horizon_hours`. seed >= 0 randomises starting positions for
    // a fresh generation; -1 reads the channel's stored seed (or no seed if
    // extending an existing schedule).
    void ensureScheduled(const std::string& channel_id,
                         std::time_t from, int horizon_hours,
                         int seed = -1);

    // Mark the earliest matching scheduled entry as aired.
    // Called by the /played endpoint after markPlayed().
    void notifyPlayed(const std::string& channel_id, const std::string& item_id);

    // XMLTV XML for all channels. Calls ensureScheduled() then reads the cache.
    std::string generateXMLTV(int horizon_hours = 24);

    // M3U channel list. Stream URLs point to base_url/channels/{id}/stream.
    std::string generateM3U(const std::string& base_url);

private:
    static std::string xmlEscape(const std::string& s);
    static std::string fmtXMLTVTime(std::time_t t);

    Database&    db_;
    RuleEngine&  engine_;
};
