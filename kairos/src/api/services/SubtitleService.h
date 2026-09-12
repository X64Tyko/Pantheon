#pragma once
#include "../IKairosService.h"
#include "../../db/SubtitleTrackRepository.h"
#include <httplib.h>

class ConfStore;
class Database;
struct ServiceContext;

// Admin-only "broken subtitle sidecar" review — see SubtitleValidation.h and
// migration v93. Small and self-contained on purpose: unlike ChapterService
// (detection, sync, writeback, several media-type-specific CRUD routes),
// there's nothing to detect or generate here — just a read-only report over
// rows SyncManager's own subtitle sidecar scan already flagged, plus a
// one-off re-check action for after the admin has fixed/replaced a file on
// disk.
class SubtitleService : public IKairosService {
public:
	explicit SubtitleService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database&                db_;
	ConfStore&                conf_;
	SubtitleTrackRepository  repo_;
};
