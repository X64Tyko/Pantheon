#pragma once
#include "ListTypes.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Database;

struct FillerItemRow {
    int64_t id = 0;
    int position = 0;
    std::string item_type, item_id, title;
    int64_t duration_ms = 0;
};

struct FillerListRow {
    std::string filler_list_id, title, advancement;
    int item_count = 0;
    int64_t total_ms = 0;
    std::optional<PlexLinkRow> plex_link;
};

struct FillerListDetail {
    std::string filler_list_id, title, advancement;
    std::vector<FillerItemRow> items;
};

class FillerRepository {
public:
	explicit FillerRepository(Database& db);

	// Returns filler_list_id.
	std::string create(const std::string& title, const std::string& advancement = "shuffle");
	void        updateField(const std::string& filler_list_id, const std::string& col,
	                         const std::string& val);
	void        remove(const std::string& filler_list_id);
	void        unlinkPlex(const std::string& filler_list_id);

	std::vector<FillerListRow>      listAll();
	std::optional<FillerListDetail> getDetail(const std::string& filler_list_id);

	// Returns {rowid, position}.
	std::pair<int64_t, int> addItem(const std::string& filler_list_id,
	                                 const std::string& item_type,
	                                 const std::string& item_id);
	// Returns count added.
	int addItems(const std::string& filler_list_id,
	             const std::vector<std::pair<std::string, std::string>>& items);
	void removeItem(int item_id);

private:
	Database& db_;
};
