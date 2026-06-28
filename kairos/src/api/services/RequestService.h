#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;

class RequestService : public IKairosService {
public:
	explicit RequestService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database& db_;
};
