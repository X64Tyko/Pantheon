#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class AuthStore;
class Database;

class AuthService : public IKairosService {
public:
	explicit AuthService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	AuthStore& auth_;
	Database&  db_;
};
