#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;
class ConfStore;

class PlaybackService : public IKairosService {
public:
	explicit PlaybackService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&  db_;
	ConfStore& conf_;
};
