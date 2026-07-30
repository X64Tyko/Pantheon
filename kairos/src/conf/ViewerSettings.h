#pragma once
#include "../db/ConfigRepository.h"
#include "../db/Database.h"
#include <string>

// Server-wide settings for a real named account's self-service channel
// building. Unlike guest_settings::channelBuilderEnabled, WHETHER a given
// account can build a channel is not decided here — that's a per-user admin
// grant (AuthUser::channel_builder_enabled, see AuthStore::
// updateChannelBuilderEnabled), since real accounts are provisioned
// individually. This namespace only holds the one setting that's genuinely
// server-wide: the quota applied to any account that's been granted access.
namespace viewer_settings
{
	inline int maxChannels(Database& db)
	{
		auto v = ConfigRepository(db).getValue("viewer_max_channels");
		if (v.empty()) return 3;
		try { return std::stoi(v); }
		catch (...) { return 3; }
	}
} // namespace viewer_settings