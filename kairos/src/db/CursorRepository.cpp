#include "CursorRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>

CursorRepository::CursorRepository(Database& db) : db_(db) {}

CursorState CursorRepository::load(const std::string& channel_id) {
    return CursorState::loadFromDB(db_, channel_id);
}

void CursorRepository::apply(const std::string& channel_id, const CursorState& state) {
    state.applyToDB(db_, channel_id);
}

void CursorRepository::clear(const std::string& channel_id) {
    CursorState::clearFromDB(db_, channel_id);
}
