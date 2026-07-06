#pragma once
#include "../IKairosService.h"
#include "../../db/ChapterRepository.h"
#include <httplib.h>

class ChapterDetectionManager;
class Database;
class SyncManager;
struct ServiceContext;

class ChapterService : public IKairosService {
public:
	ChapterService(const ServiceContext& ctx, ChapterDetectionManager& detector);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&                db_;
	SyncManager&              sync_;
	ChapterRepository         repo_;
	ChapterDetectionManager&  detector_;
};
