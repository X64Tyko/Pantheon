#include "Router.h"
#include "AuthContext.h"
#include "RouteHelpers.h"
#include "ServiceContext.h"
#include "auth/AuthStore.h"
#include "conf/ConfStore.h"
#include "db/Database.h"
#include "download/DownloadManager.h"
#include "log/LogBuffer.h"
#include "scheduler/EPGMaterializer.h"
#include "scheduler/RuleEngine.h"
#include "source/SyncManager.h"
#include "services/ActivityService.h"
#include "services/ArrService.h"
#include "services/AuthService.h"
#include "services/BlockService.h"
#include "services/ChannelService.h"
#include "services/ConfigService.h"
#include "services/ContentService.h"
#include "services/DownloadService.h"
#include "services/FillerService.h"
#include "services/PlaylistService.h"
#include "services/SchedulerService.h"
#include "services/SourceService.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

static bool isPublicPath(const std::string& path) {
	if (!path.starts_with("/api/")) return true;
	if (path == "/api/auth/setup") return true;
	if (path == "/api/auth/login") return true;
	if (path.ends_with("/now") || path.ends_with("/next") || path.ends_with("/epg"))
		return true;
	return false;
}

Router::Router(httplib::Server& svr, Database& db, SyncManager& sync,
               ConfStore& conf, LogBuffer& logs,
               RuleEngine& engine, EPGMaterializer& materializer,
               DownloadManager& dl, AuthStore& auth)
	: svr_(svr), db_(db), sync_(sync), conf_(conf), logs_(logs),
	  engine_(engine), materializer_(materializer), dl_(dl), auth_(auth),
	  schedule_cache_(db)
{}

Router::~Router() = default;

void Router::registerRoutes() {
#ifdef KAIROS_DEV
	svr_.Options(".*", [](const Req&, Res& res) {
		res.set_header("Access-Control-Allow-Origin",  "*");
		res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
		res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
		res.status = 204;
	});
	svr_.set_post_routing_handler([](const Req&, Res& res) {
		res.set_header("Access-Control-Allow-Origin", "*");
	});
#endif

	svr_.set_pre_routing_handler([this](const Req& req, Res& res)
	    -> httplib::Server::HandlerResponse
	{
		conf_.maybeReload();
		clearCurrentUser();

		if (isPublicPath(req.path)) return httplib::Server::HandlerResponse::Unhandled;

		std::string token;
		if (req.has_header("Authorization")) {
			const std::string& hdr = req.get_header_value("Authorization");
			if (hdr.starts_with("Bearer ")) token = hdr.substr(7);
		} else {
			auto it = req.params.find("token");
			if (it != req.params.end()) token = it->second;
		}

		auto user = auth_.validate(token);
		if (!user) {
			res.status = 401;
			res.set_content(R"({"error":"Unauthorized"})", "application/json");
			return httplib::Server::HandlerResponse::Handled;
		}
		setCurrentUser(std::move(user));
		return httplib::Server::HandlerResponse::Unhandled;
	});

	ServiceContext ctx{db_, conf_, sync_, schedule_cache_,
	                   materializer_, engine_, auth_, logs_, dl_};

	services_.push_back(std::make_unique<AuthService>(ctx));
	services_.push_back(std::make_unique<SourceService>(ctx));
	services_.push_back(std::make_unique<ConfigService>(ctx));
	services_.push_back(std::make_unique<ArrService>(ctx));
	services_.push_back(std::make_unique<ChannelService>(ctx));
	services_.push_back(std::make_unique<BlockService>(ctx));
	services_.push_back(std::make_unique<ContentService>(ctx));
	services_.push_back(std::make_unique<PlaylistService>(ctx));
	services_.push_back(std::make_unique<FillerService>(ctx));
	services_.push_back(std::make_unique<ActivityService>(ctx));
	services_.push_back(std::make_unique<DownloadService>(ctx));
	services_.push_back(std::make_unique<SchedulerService>(ctx));

	for (auto& svc : services_) svc->registerRoutes(svr_);

	svr_.Get("/api/sync/status", [this](const Req&, Res& res) {
		route::ok(res, json{{"running", sync_.isSyncing()}}.dump());
	});

	svr_.set_mount_point("/", "./ui-dist");
	svr_.Get(".*", [](const Req&, Res& res) {
		std::ifstream ifs("./ui-dist/index.html");
		if (!ifs) { res.status = 404; return; }
		std::string html((std::istreambuf_iterator<char>(ifs)), {});
		res.set_content(html, "text/html; charset=utf-8");
	});
}
