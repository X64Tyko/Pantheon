#include "TimeslotRepository.h"
#include "DbHelpers.h"
#include <SQLiteCpp/SQLiteCpp.h>

TimeslotRepository::TimeslotRepository(Database& db) : db_(db) {}

// ── Slots ─────────────────────────────────────────────────────────────────────

std::string TimeslotRepository::createSlot(const std::string& block_id, const nlohmann::json& d) {
    int next_index = 0;
    {
        SQLite::Statement q(db_.get(),
            "SELECT COALESCE(MAX(slot_index)+1,0) FROM timeslot_slot WHERE block_id=?");
        q.bind(1, block_id);
        if (q.executeStep()) next_index = q.getColumn(0).getInt();
    }
    std::string slot_id = db::generateId();
    SQLite::Statement s(db_.get(), R"(
        INSERT INTO timeslot_slot
            (slot_id, block_id, slot_index, slot_offset_mins, slot_duration_mins,
             overflow, late_start_mins, early_start_secs, align_to_mins, start_scope)
        VALUES (?,?,?,?,?,?,?,?,?,?)
    )");
    s.bind(1, slot_id);
    s.bind(2, block_id);
    s.bind(3, d.value("slot_index", next_index));
    s.bind(4, d.value("slot_offset_mins", 0));
    s.bind(5, d.value("slot_duration_mins", 60));
    s.bind(6, d.value("overflow", std::string("cutoff")));
    s.bind(7, d.value("late_start_mins", 5));
    s.bind(8, d.value("early_start_secs", 0));
    s.bind(9, d.value("align_to_mins", 0));
    s.bind(10, d.value("start_scope", std::string("block")));
    s.exec();
    return slot_id;
}

void TimeslotRepository::updateSlotField(const std::string& slot_id,
                                          const std::string& col, const std::string& val) {
    SQLite::Statement s(db_.get(), "UPDATE timeslot_slot SET " + col + "=? WHERE slot_id=?");
    s.bind(1, val); s.bind(2, slot_id); s.exec();
}

void TimeslotRepository::updateSlotField(const std::string& slot_id,
                                          const std::string& col, int val) {
    SQLite::Statement s(db_.get(), "UPDATE timeslot_slot SET " + col + "=? WHERE slot_id=?");
    s.bind(1, val); s.bind(2, slot_id); s.exec();
}

void TimeslotRepository::deleteSlot(const std::string& slot_id) {
    SQLite::Statement s(db_.get(), "DELETE FROM timeslot_slot WHERE slot_id=?");
    s.bind(1, slot_id); s.exec();
}

void TimeslotRepository::reorderSlots(const std::string& block_id,
                                       const std::vector<std::string>& ordered_ids) {
    SQLite::Transaction txn(db_.get());
    SQLite::Statement s(db_.get(),
        "UPDATE timeslot_slot SET slot_index=? WHERE slot_id=? AND block_id=?");
    for (int i = 0; i < static_cast<int>(ordered_ids.size()); ++i) {
        s.bind(1, i);
        s.bind(2, ordered_ids[i]);
        s.bind(3, block_id);
        s.exec();
        s.reset();
    }
    txn.commit();
}

void TimeslotRepository::resetSlotCursor(const std::string& slot_id) {
    SQLite::Statement s(db_.get(),
        "UPDATE timeslot_slot SET queue_pos=0, episode_pos=0 WHERE slot_id=?");
    s.bind(1, slot_id); s.exec();
}

// ── Queue entries ─────────────────────────────────────────────────────────────

std::string TimeslotRepository::addQueueEntry(const std::string& slot_id,
                                               const nlohmann::json& d) {
    int next_index = 0;
    {
        SQLite::Statement q(db_.get(),
            "SELECT COALESCE(MAX(queue_index)+1,0) FROM timeslot_slot_queue WHERE slot_id=?");
        q.bind(1, slot_id);
        if (q.executeStep()) next_index = q.getColumn(0).getInt();
    }
    std::string entry_id = db::generateId();
    std::string premiere = d.value("premiere_date", std::string(""));
    SQLite::Statement s(db_.get(), R"(
        INSERT INTO timeslot_slot_queue
            (entry_id, slot_id, queue_index, content_type, content_id,
             premiere_date, pre_premiere_behavior)
        VALUES (?,?,?,?,?,?,?)
    )");
    s.bind(1, entry_id);
    s.bind(2, slot_id);
    s.bind(3, d.value("queue_index", next_index));
    s.bind(4, d.value("content_type", std::string("show")));
    s.bind(5, d.value("content_id",   std::string("")));
    if (premiere.empty()) s.bind(6); else s.bind(6, premiere);
    s.bind(7, d.value("pre_premiere_behavior", std::string("replay_previous")));
    s.exec();
    return entry_id;
}

void TimeslotRepository::updateQueueEntry(const std::string& entry_id,
                                           const std::string& col, const std::string& val) {
    if (col == "premiere_date" && val.empty()) {
        SQLite::Statement s(db_.get(),
            "UPDATE timeslot_slot_queue SET premiere_date=NULL WHERE entry_id=?");
        s.bind(1, entry_id); s.exec();
    } else {
        SQLite::Statement s(db_.get(),
            "UPDATE timeslot_slot_queue SET " + col + "=? WHERE entry_id=?");
        s.bind(1, val); s.bind(2, entry_id); s.exec();
    }
}

void TimeslotRepository::deleteQueueEntry(const std::string& entry_id) {
    std::string slot_id;
    {
        SQLite::Statement q(db_.get(),
            "SELECT slot_id FROM timeslot_slot_queue WHERE entry_id=?");
        q.bind(1, entry_id);
        if (!q.executeStep()) return;
        slot_id = q.getColumn(0).getString();
    }
    SQLite::Transaction txn(db_.get());
    SQLite::Statement d(db_.get(),
        "DELETE FROM timeslot_slot_queue WHERE entry_id=?");
    d.bind(1, entry_id); d.exec();
    // Compact queue_index
    SQLite::Statement renum(db_.get(), R"(
        UPDATE timeslot_slot_queue
        SET queue_index = (
            SELECT COUNT(*) FROM timeslot_slot_queue t2
            WHERE t2.slot_id = timeslot_slot_queue.slot_id
              AND t2.queue_index < timeslot_slot_queue.queue_index
        )
        WHERE slot_id = ?
    )");
    renum.bind(1, slot_id); renum.exec();
    txn.commit();
}

void TimeslotRepository::reorderQueueEntries(const std::string& slot_id,
                                              const std::vector<std::string>& ordered_ids) {
    SQLite::Transaction txn(db_.get());
    SQLite::Statement s(db_.get(),
        "UPDATE timeslot_slot_queue SET queue_index=? WHERE entry_id=? AND slot_id=?");
    for (int i = 0; i < static_cast<int>(ordered_ids.size()); ++i) {
        s.bind(1, i);
        s.bind(2, ordered_ids[i]);
        s.bind(3, slot_id);
        s.exec();
        s.reset();
    }
    txn.commit();
}

// ── Validation ────────────────────────────────────────────────────────────────

bool TimeslotRepository::slotBelongsToBlock(const std::string& slot_id,
                                             const std::string& block_id) {
    SQLite::Statement q(db_.get(),
        "SELECT 1 FROM timeslot_slot WHERE slot_id=? AND block_id=?");
    q.bind(1, slot_id); q.bind(2, block_id);
    return q.executeStep();
}

bool TimeslotRepository::entryBelongsToSlot(const std::string& entry_id,
                                              const std::string& slot_id) {
    SQLite::Statement q(db_.get(),
        "SELECT 1 FROM timeslot_slot_queue WHERE entry_id=? AND slot_id=?");
    q.bind(1, entry_id); q.bind(2, slot_id);
    return q.executeStep();
}

void TimeslotRepository::removeExhaustedQueueEntry(const std::string& entry_id,
                                                    const std::string& slot_id) {
    { SQLite::Statement s(db_.get(),
          "DELETE FROM timeslot_slot_queue WHERE entry_id=?");
      s.bind(1, entry_id); s.exec(); }
    SQLite::Statement renum(db_.get(), R"(
        UPDATE timeslot_slot_queue
        SET queue_index = (
            SELECT COUNT(*) FROM timeslot_slot_queue t2
            WHERE t2.slot_id = timeslot_slot_queue.slot_id
              AND t2.queue_index < timeslot_slot_queue.queue_index
        )
        WHERE slot_id = ?
    )");
    renum.bind(1, slot_id); renum.exec();
}
