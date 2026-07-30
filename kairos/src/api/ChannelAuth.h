#pragma once
#include "../util/RateLimiter.h"
#include <string>

class Database;
struct AuthUser;

// Ownership-based authorization for the channel builder — admin always
// passes; a guest or real-viewer account passes only for a channel it owns
// (channel.owner_user_id, see migration v101). Kept as narrow single-row
// lookups (not a full Channel/Block load via ChannelRepository) to keep
// this header's dependency footprint to Database.h + SQLiteCpp only, same
// reasoning as TimeslotRepository::slotBelongsToBlock.
namespace channel_auth
{
	// True if `user` may create/modify/delete `channel_id` or anything
	// nested under it (blocks, timeslots, filler entries, bumpers). False if
	// the channel doesn't exist — callers should treat that as 404, not 403.
	bool canEditChannel(Database& db, const AuthUser& user, const std::string& channel_id);

	// Same check, resolved from a block_id — BlockService/TimeslotService
	// routes only carry block_id (not channel_id) in their path.
	bool canEditChannelForBlock(Database& db, const AuthUser& user, const std::string& block_id);

	// canEditChannel plus per-user rate limiting for non-admin mutation
	// traffic (no-op for admin, which has no cap) — the combination every
	// guest/real-viewer-reachable mutation route actually wants. `limiter`
	// is the one shared instance on ServiceContext (see Router.h's
	// guest_mutation_limiter_) so a caller can't multiply their effective
	// rate by spreading calls across ChannelService/BlockService/
	// TimeslotService.
	bool canMutateChannel(Database& db, RateLimiter& limiter, const AuthUser& user,
						  const std::string& channel_id);
	bool canMutateChannelForBlock(Database& db, RateLimiter& limiter, const AuthUser& user,
								  const std::string& block_id);
}