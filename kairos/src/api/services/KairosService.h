#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;

class KairosService : public IKairosService {
public:
	explicit KairosService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database& db_;
};
