#pragma once
#include "GuestSettings.h"
#include "../db/ConfigRepository.h"
#include "../db/Database.h"

// General security-hardening settings — same ConfigRepository key/value
// pattern as GuestSettings.h. Not guest-specific on its own (an admin can
// turn this on regardless of guest profiles), but enabling guest profiles
// auto-enables it as a safe default (see ConfigService.cpp's
// PATCH /api/config/settings) since a device holding a real admin session
// is reachable by the same "who's watching?" picker every other profile
// is — including a guest's — once guest accounts exist at all.
namespace security_settings
{
	inline bool requireAdminPasswordSwitchRaw(Database& db)
	{
		return ConfigRepository(db).getValue("require_admin_password_switch") == "1";
	}

	// The actual enforced value: explicitly on, OR guest profiles are
	// enabled. Deliberately not just "whatever the raw setting says" — this
	// is enforced here (not just nudged by auto-enabling the raw setting) so
	// it can't be quietly weakened by flipping the raw setting back off
	// while guest profiles remain on, the same "enforced here, not just
	// nudged in the UI" posture AuthStore::switchProfile's own no-pin-admin
	// rule already uses.
	inline bool requireAdminPasswordSwitchEffective(Database& db)
	{
		return requireAdminPasswordSwitchRaw(db) || guest_settings::enabled(db);
	}
} // namespace security_settings