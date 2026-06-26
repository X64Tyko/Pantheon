#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class Database;

class FillerRepository {
public:
	explicit FillerRepository(Database& db);

	// Returns filler_list_id.
	std::string create(const std::string& title, const std::string& advancement = "shuffle");
	void        updateField(const std::string& filler_list_id, const std::string& col,
	                         const std::string& val);
	void        remove(const std::string& filler_list_id);
	void        unlinkPlex(const std::string& filler_list_id);

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
