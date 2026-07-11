#pragma once
#include <string>
#include <vector>

class Database;

struct DuplicateCandidateRow {
    std::string candidate_id;
    std::string item_type; // "show" | "movie"
    std::string kairos_id_a, kairos_id_b;
    std::string title_a, title_b;
    int         year_a = 0, year_b = 0;
    std::string thumb_a, thumb_b;
    std::string trigger; // "fuzzy_title" | "folder_uncertain" | "both"
    std::string reason;
    double      title_similarity = 0;
    std::string folder_a, folder_b;
    std::string created_at;
};

// Backing store for the "Duplicates" review queue — candidate pairs flagged
// by SyncManager's sync-time dedup as an uncertain match (fuzzy title, or a
// folder match that only succeeded via a case-insensitive fallback), for a
// human to Merge (via the existing /shows|movies/:id/merge routes) or
// dismiss as "Not a duplicate". See the duplicate_candidate table's schema
// comment (Database.cpp v74 migration) for how dismissal is remembered —
// merging or dismissing here doesn't itself resolve the row on merge;
// ContentRepository::mergeShowInto/mergeMovieInto deletes it as a side
// effect of the merge.
class DuplicateRepository {
public:
    explicit DuplicateRepository(Database& db);

    // item_type_filter: "" for both, "show", or "movie".
    std::vector<DuplicateCandidateRow> listPending(const std::string& item_type_filter,
                                                    int limit, int offset) const;
    int countPending(const std::string& item_type_filter) const;

    // False if not found or already decided (dismissed).
    bool dismiss(const std::string& candidate_id);

private:
    Database& db_;
};
