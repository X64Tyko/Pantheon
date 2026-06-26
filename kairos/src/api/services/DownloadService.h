#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class ConfStore;
class DownloadManager;

class DownloadService : public IKairosService {
public:
	explicit DownloadService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	ConfStore&       conf_;
	DownloadManager& dl_;
};
