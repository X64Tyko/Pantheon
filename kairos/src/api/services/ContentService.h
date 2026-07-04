#pragma once
#include "../IKairosService.h"
#include <httplib.h>
#include <string>

class ConfStore;
class Database;
class SyncManager;
class ScraperManager;
struct ServiceContext;

class ContentService : public IKairosService {
public:
	ContentService(const ServiceContext& ctx, ScraperManager& scraper);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&       db_;
	ConfStore&      conf_;
	SyncManager&    sync_;
	ScraperManager& scraper_;

	void proxyImage(const httplib::Request& req, const std::string& imgPath,
	                const std::string& sourceId, httplib::Response& res);
};
