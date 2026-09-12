#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;
class ScheduleCache;
class SyncManager;

class FillerService : public IKairosService {
public:
	explicit FillerService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	// Clears every channel that references filler_list_id, directly (channel-level
	// filler pool) or indirectly (a block's filler pool, or as regular block
	// content) — a filler_list's contents aren't scoped to one channel.
	void clearChannelsUsingFillerList(const std::string& filler_list_id);

	Database&      db_;
	SyncManager&   sync_;
	ScheduleCache& schedule_cache_;
};
