#include "ChannelAuth.h"
#include "../auth/AuthStore.h"
#include "../db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdint>

namespace
{
	// A single interactive Save in the channel builder (ChannelDetailStore::
	// saveChannel(), Hades) fires one HTTP call per block create/update/
	// delete, per content item add/update/remove, per filler entry, per
	// timeslot slot, and per queue entry — a real schedule with a few dozen
	// blocks can easily produce several hundred calls in one Save. This just
	// needs to blunt a scripted flood, not cap legitimate interactive use
	// (ownership is the real security boundary — this only bounds request
	// volume), so the ceiling is deliberately generous.
	constexpr int kMutationRateLimitCount          = 2000;
	constexpr int64_t kMutationRateLimitWindowSecs = 60;
}

namespace channel_auth
{
	bool canEditChannel(Database& db, const AuthUser& user, const std::string& channel_id)
	{
		if (user.role == "admin") return true;
		SQLite::Statement q(db.get(), "SELECT owner_user_id FROM channel WHERE channel_id = ?");
		q.bind(1, channel_id);
		if (!q.executeStep()) return false;
		return !q.getColumn(0).isNull() && q.getColumn(0).getString() == user.user_id;
	}

	bool canEditChannelForBlock(Database& db, const AuthUser& user, const std::string& block_id)
	{
		if (user.role == "admin") return true;
		SQLite::Statement q(db.get(), "SELECT channel_id FROM block WHERE block_id = ?");
		q.bind(1, block_id);
		if (!q.executeStep()) return false;
		return canEditChannel(db, user, q.getColumn(0).getString());
	}

	bool canMutateChannel(Database& db, RateLimiter& limiter, const AuthUser& user,
						  const std::string& channel_id)
	{
		if (user.role == "admin") return true;
		// Rate limit (cheap, in-memory) before the DB ownership lookup.
		if (!limiter.allow(user.user_id, kMutationRateLimitCount, kMutationRateLimitWindowSecs)) return false;
		return canEditChannel(db, user, channel_id);
	}

	bool canMutateChannelForBlock(Database& db, RateLimiter& limiter, const AuthUser& user,
								  const std::string& block_id)
	{
		if (user.role == "admin") return true;
		if (!limiter.allow(user.user_id, kMutationRateLimitCount, kMutationRateLimitWindowSecs)) return false;
		return canEditChannelForBlock(db, user, block_id);
	}
}