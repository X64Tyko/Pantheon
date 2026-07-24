#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;

// Watch Together's identity/discovery half — see Database.cpp's v95 migration
// comment for why this owns persistence and the Home shelf feed but no live
// playback state (position/paused), which stays entirely in Hermes'
// in-memory WatchTogetherSession once that lands.
class WatchTogetherService : public IKairosService {
public:
	explicit WatchTogetherService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database& db_;
};
