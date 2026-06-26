#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;

class ArrService : public IKairosService {
public:
	explicit ArrService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database& db_;
};
