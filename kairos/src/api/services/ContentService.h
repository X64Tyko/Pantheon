#pragma once
#include "../IKairosService.h"
#include "../../model/WritebackFields.h"
#include <httplib.h>
#include <optional>
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

	// Resolves imgPath (CDN URL / source-relative path / "local:" sentinel —
	// same three forms proxyImage() serves) into raw bytes for writeback
	// upload. Unlike proxyImage this is a synchronous, uncached fetch — it's
	// only called from an admin-triggered writeback action, not hot-path
	// image traffic, so the on-disk image-cache machinery would be
	// pointless overhead here.
	std::optional<WritebackImage> fetchImageBytes(const std::string& imgPath,
	                                               const std::string& sourceId);
};
