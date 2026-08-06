#pragma once
#include <cstdint>
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include "../model/Channel.h"

class Database;

class ChannelRepository
{
public:
	explicit ChannelRepository(Database& db);

	// viewer_user_id: unset for an unauthenticated/internal caller (public
	// M3U/XMLTV consumers) — returns only real (non-demo) channels. Set for
	// an authenticated caller — also includes that caller's own demo
	// channel(s), if any. Admin callers should pass std::nullopt for
	// as_admin=false... see the `as_admin` param instead: when true, returns
	// every channel unfiltered (admin retains full visibility, same as today).
	std::vector<Channel> listChannels(const std::optional<std::string>& viewer_user_id = std::nullopt,
									  bool as_admin                                    = false);
	std::optional<Channel> findById(const std::string& channel_id);

	std::string create(const std::string& name, int number,
					   const std::string& timezone,
					   const std::optional<std::string>& owner_user_id = std::nullopt,
					   bool is_demo                                    = false);

	// Count of self-service (owner_user_id = user_id) channels, used for the
	// guest/viewer creation quota.
	int countOwnedBy(const std::string& user_id);

	void updateField(const std::string& channel_id, const std::string& col,
					 const std::string& value);
	void updateField(const std::string& channel_id, const std::string& col, int value);

	void remove(const std::string& channel_id);

	// Channel filler entry CRUD
	// Returns JSON array: [{id, content_type, content_id, title, advancement, weight, position, season_filter?}]
	nlohmann::json listFillerEntries(const std::string& channel_id);

	struct FillerEntryResult
	{
		int64_t id;
		int position;
		std::string title;
	};

	FillerEntryResult addFillerEntry(const std::string& channel_id,
									 const std::string& content_type,
									 const std::string& content_id,
									 const std::string& advancement,
									 int weight,
									 std::optional<int> season_filter);

	void updateFillerEntryField(int id, const std::string& col, const std::string& val);
	void updateFillerEntryField(int id, const std::string& col, int val);
	void removeFillerEntry(int id);

	std::optional<std::string> getAnchorHashes(const std::string& channel_id);

private:
	Database& db_;

	static Channel rowToChannel(SQLite::Statement& q);
};