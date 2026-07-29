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
} // namespace guest_settings