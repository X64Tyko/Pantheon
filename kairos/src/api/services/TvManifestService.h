#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class Database;

// GET /api/tv/manifest — backs the shared Home/Library/Detail/Guide layout
// contract consumed identically by Hades' /tv route and (eventually) native
// clients. See kairos/src/db/Database.cpp's v81 migration comment for the
// tv_shelf/tv_zone tables this reads, and hades/src/tv/ for the client-side
// consumer. Public, unauthenticated — see Router.cpp's isPublicPath, same
// "structural data, not per-user" reasoning as /api/config/public-settings.
class TvManifestService : public IKairosService {
public:
	explicit TvManifestService(const ServiceContext& ctx);
	void registerRoutes(httplib::Server& svr) override;

private:
	Database& db_;
};
