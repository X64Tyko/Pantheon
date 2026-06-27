#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include "Database.h"

class TimeslotRepository {
public:
    explicit TimeslotRepository(Database& db);

    // Slots
    std::string createSlot(const std::string& block_id, const nlohmann::json& data);
    void        updateSlotField(const std::string& slot_id, const std::string& col, const std::string& val);
    void        updateSlotField(const std::string& slot_id, const std::string& col, int val);
    void        deleteSlot(const std::string& slot_id);
    void        reorderSlots(const std::string& block_id, const std::vector<std::string>& ordered_ids);
    void        resetSlotCursor(const std::string& slot_id);

    // Queue entries
    std::string addQueueEntry(const std::string& slot_id, const nlohmann::json& data);
    void        updateQueueEntry(const std::string& entry_id, const std::string& col, const std::string& val);
    void        deleteQueueEntry(const std::string& entry_id);
    void        reorderQueueEntries(const std::string& slot_id, const std::vector<std::string>& ordered_ids);

    // Validation
    bool slotBelongsToBlock(const std::string& slot_id, const std::string& block_id);
    bool entryBelongsToSlot(const std::string& entry_id, const std::string& slot_id);

private:
    Database& db_;
};
