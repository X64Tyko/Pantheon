#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;
class ScraperManager;

class KairosService : public IKairosService {
public:
	KairosService(const ServiceContext& ctx, ScraperManager& scraper);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&       db_;
	ScraperManager& scraper_;
};
