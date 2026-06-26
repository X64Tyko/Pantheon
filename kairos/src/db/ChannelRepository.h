#pragma once
#include <cstdint>
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
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

	void updateField(const std::string& channel_id, const std::string& col,
	                 const std::string& value);
	void updateField(const std::string& channel_id, const std::string& col, int value);

	void remove(const std::string& channel_id);

	// Channel filler entry CRUD
	// Returns JSON array: [{id, content_type, content_id, title, advancement, weight, position, season_filter?}]
	nlohmann::json listFillerEntries(const std::string& channel_id);

	struct FillerEntryResult { int64_t id; int position; std::string title; };
	FillerEntryResult addFillerEntry(const std::string& channel_id,
	                                  const std::string& content_type,
	                                  const std::string& content_id,
	                                  const std::string& advancement,
	                                  int weight,
	                                  std::optional<int> season_filter);

	void updateFillerEntryField(int id, const std::string& col, const std::string& val);
	void updateFillerEntryField(int id, const std::string& col, int val);
	void removeFillerEntry(int id);

private:
	Database& db_;

	static Channel rowToChannel(SQLite::Statement& q);
};
