#include "DuplicateRepository.h"
#include "Database.h"
#include <SQLiteCpp/SQLiteCpp.h>

DuplicateRepository::DuplicateRepository(Database& db) : db_(db) {}

namespace {

// INNER JOINs against the live show/movie table on both sides — a candidate
// naming an id that's since been deleted (should already be cleaned up by
// the merge/orphan paths, but defensively) simply doesn't show up rather
// than crashing on a missing title/thumb.
void listForType(Database& db, const std::string& item_type, int limit, int offset,
                  std::vector<DuplicateCandidateRow>& out) {
    const std::string table = item_type; // "show" | "movie"
    const std::string id_col = item_type + "_id";
    SQLite::Statement q(db.get(), R"(
        SELECT dc.candidate_id, dc.kairos_id_a, dc.kairos_id_b,
               a.title, COALESCE(a.year,0), a.thumb,
               b.title, COALESCE(b.year,0), b.thumb,
               dc.trigger, dc.reason, dc.title_similarity, dc.folder_a, dc.folder_b, dc.created_at
        FROM duplicate_candidate dc
        JOIN )" + table + R"( a ON a.)" + id_col + R"( = dc.kairos_id_a
        JOIN )" + table + R"( b ON b.)" + id_col + R"( = dc.kairos_id_b
        WHERE dc.status='pending' AND dc.item_type=?
        ORDER BY dc.created_at DESC
        LIMIT ? OFFSET ?
    )");
    q.bind(1, item_type);
    q.bind(2, limit);
    q.bind(3, offset);
    while (q.executeStep()) {
        DuplicateCandidateRow r;
        r.candidate_id     = q.getColumn(0).getString();
        r.item_type        = item_type;
        r.kairos_id_a       = q.getColumn(1).getString();
        r.kairos_id_b       = q.getColumn(2).getString();
        r.title_a           = q.getColumn(3).getString();
        r.year_a            = q.getColumn(4).getInt();
        r.thumb_a           = q.getColumn(5).getString();
        r.title_b           = q.getColumn(6).getString();
        r.year_b            = q.getColumn(7).getInt();
        r.thumb_b           = q.getColumn(8).getString();
        r.trigger           = q.getColumn(9).getString();
        r.reason            = q.getColumn(10).getString();
        r.title_similarity  = q.getColumn(11).getDouble();
        r.folder_a          = q.getColumn(12).getString();
        r.folder_b          = q.getColumn(13).getString();
        r.created_at        = q.getColumn(14).getString();
        out.push_back(std::move(r));
    }
}

int countForType(Database& db, const std::string& item_type) {
    SQLite::Statement q(db.get(),
        "SELECT COUNT(*) FROM duplicate_candidate WHERE status='pending' AND item_type=?");
    q.bind(1, item_type);
    return q.executeStep() ? q.getColumn(0).getInt() : 0;
}

} // namespace

std::vector<DuplicateCandidateRow> DuplicateRepository::listPending(
    const std::string& item_type_filter, int limit, int offset) const {
    std::vector<DuplicateCandidateRow> out;
    if (item_type_filter.empty() || item_type_filter == "show")
        listForType(db_, "show", limit, offset, out);
    if (item_type_filter.empty() || item_type_filter == "movie")
        listForType(db_, "movie", limit, offset, out);
    return out;
}

int DuplicateRepository::countPending(const std::string& item_type_filter) const {
    if (item_type_filter == "show")  return countForType(db_, "show");
    if (item_type_filter == "movie") return countForType(db_, "movie");
    return countForType(db_, "show") + countForType(db_, "movie");
}

bool DuplicateRepository::dismiss(const std::string& candidate_id) {
    SQLite::Statement rd(db_.get(),
        "SELECT status FROM duplicate_candidate WHERE candidate_id=?");
    rd.bind(1, candidate_id);
    if (!rd.executeStep() || rd.getColumn(0).getString() != "pending") return false;

    SQLite::Statement upd(db_.get(), R"(
        UPDATE duplicate_candidate
        SET status='dismissed', decided_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
        WHERE candidate_id=?
    )");
    upd.bind(1, candidate_id);
    upd.exec();
    return true;
}
