#pragma once
#include <httplib.h>

class IKairosService {
public:
	virtual ~IKairosService() = default;
	virtual void registerRoutes(httplib::Server& svr) = 0;
};
