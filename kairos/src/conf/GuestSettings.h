#pragma once
#include "../db/ConfigRepository.h"
#include "../db/Database.h"
#include <string>

// Guest-profile settings — same ConfigRepository key/value + "empty means
// unconfigured, apply the documented default" pattern ConfigService.cpp's
// castAppId/defaultLandingPage use for their own settings. Pulled into one
// shared header (rather than each reader defining its own copy, the way
// castAppId/defaultLandingPage currently only ever have one reader each) since
// these three keys are read from two different places — ConfigService.cpp's
// admin-facing GET/PATCH /api/config/settings and public-settings, and
// AuthService.cpp's guest routes themselves — and a default value drifting
// out of sync between two independent copies would be a real, easy-to-miss bug.
namespace guest_settings
{
	inline bool enabled(Database& db)
	{
		return ConfigRepository(db).getValue("guest_profiles_enabled") == "1";
	}

	inline int maxConcurrent(Database& db)
	{
		auto v = ConfigRepository(db).getValue("guest_max_concurrent");
		if (v.empty()) return 20;
		try { return std::stoi(v); }
		catch (...) { return 20; }
	}

	inline int idleTimeoutDays(Database& db)
	{
		auto v = ConfigRepository(db).getValue("guest_idle_timeout_days");
		if (v.empty()) return 7;
		try { return std::stoi(v); }
		catch (...) { return 7; }
	}

	// Guest access to the channel builder — a separate opt-in from
	// guest_profiles_enabled itself, and only meaningful (and only
	// persistable as true, see ConfigService.cpp's PATCH validation) while
	// that's also on. Guests get a throwaway demo channel built against the
	// real library, same as any other viewer — no separate curated-library
	// scoping, since a public demo instance is expected to run as its own
	// separate, already-curated deployment rather than sharing a personal
	// server's real library. Contrast with a real named account's
	// channel-builder access, which is a per-user admin grant
	// (AuthUser::channel_builder_enabled), not a server-wide setting like
	// this one — guests aren't individually provisioned, so there's no
	// account to grant it on.
	inline bool channelBuilderEnabled(Database& db)
	{
		return ConfigRepository(db).getValue("guest_channel_builder_enabled") == "1";
	}

	inline int maxDemoChannels(Database& db)
	{
		auto v = ConfigRepository(db).getValue("guest_max_demo_channels");
		if (v.empty()) return 1;
		try { return std::stoi(v); }
		catch (...) { return 1; }
	}
} // namespace guest_settings