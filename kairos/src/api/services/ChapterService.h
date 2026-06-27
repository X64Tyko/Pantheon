#pragma once
#include "../IKairosService.h"
#include "../../db/ChapterRepository.h"
#include <httplib.h>

class Database;
class SyncManager;
struct ServiceContext;

class ChapterService : public IKairosService {
public:
	explicit ChapterService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&         db_;
	SyncManager&      sync_;
	ChapterRepository repo_;
};
