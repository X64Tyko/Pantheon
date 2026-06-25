#pragma once
#include <cstdint>
#include <SQLiteCpp/SQLiteCpp.h>
#include <optional>
#include <string>
#include <vector>
#include "../model/Channel.h"

class Database;

class ChannelRepository {
public:
    explicit ChannelRepository(Database& db);

    std::vector<Channel> listChannels();
    std::optional<Channel> findById(const std::string& channel_id);

    std::string create(const std::string& name, int number,
                       const std::string& timezone, const std::string& advance_mode);

    // Single-field text update (maps to PATCH /api/channels/:id).
    void updateField(const std::string& channel_id, const std::string& col,
                     const std::string& value);
    void updateField(const std::string& channel_id, const std::string& col, int value);

    void remove(const std::string& channel_id);

private:
    Database& db_;

    static Channel rowToChannel(SQLite::Statement& q);
};
