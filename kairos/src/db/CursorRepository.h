#pragma once
#include <string>
#include "../scheduler/CursorState.h"

class Database;

class CursorRepository {
public:
    explicit CursorRepository(Database& db);

    CursorState load(const std::string& channel_id);
    void        apply(const std::string& channel_id, const CursorState& state);
    void        clear(const std::string& channel_id);

private:
    Database& db_;
};
