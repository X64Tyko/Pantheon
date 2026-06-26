#pragma once
#include "../IKairosService.h"
#include <httplib.h>
#include <string>

class ConfStore;
class Database;
struct ServiceContext;

class ContentService : public IKairosService {
public:
	explicit ContentService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&  db_;
	ConfStore& conf_;

	void proxyImage(const std::string& imgPath, const std::string& sourceId,
	                httplib::Response& res);
};
