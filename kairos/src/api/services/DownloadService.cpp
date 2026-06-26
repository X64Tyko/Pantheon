#include "DownloadService.h"
#include "../RouteHelpers.h"
#include "../../conf/ConfStore.h"
#include "../../download/DownloadManager.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

DownloadService::DownloadService(const ServiceContext& ctx)
	: conf_(ctx.conf), dl_(ctx.dl) {}

void DownloadService::registerRoutes(httplib::Server& svr) {

	svr.Get("/api/config/download", [this](const Req&, Res& res) {
		route::ok(res, json{{"path", conf_.getDownloadPath()}}.dump());
	});

	svr.Put("/api/config/download", [this](const Req& req, Res& res) {
		try {
			auto b    = json::parse(req.body);
			auto path = b.value("path", "");
			conf_.setDownloadPath(path);
			route::ok(res, json{{"ok", true}}.dump());
		} catch (const json::exception& e) { route::err(res, 400, e.what()); }
	});

	svr.Post("/api/download/jobs", [this](const Req& req, Res& res) {
		try {
			auto b    = json::parse(req.body);
			auto url  = b.value("url", "");
			auto path = b.contains("path") && b["path"].is_string() && !b["path"].get<std::string>().empty()
			                ? b["path"].get<std::string>()
			                : conf_.getDownloadPath();
			if (url.empty())  { route::err(res, 400, "url required");                 return; }
			if (path.empty()) { route::err(res, 400, "download path not configured"); return; }
			auto id = dl_.startJob(url, path);
			route::ok(res, json{{"job_id", id}}.dump());
		} catch (const json::exception& e) { route::err(res, 400, e.what()); }
	});

	svr.Get("/api/download/jobs", [this](const Req&, Res& res) {
		auto jobs = dl_.getJobs();
		json result = json::array();
		for (const auto& j : jobs) {
			json log = json::array();
			for (const auto& line : j.log) log.push_back(line);
			result.push_back({
				{"id",         j.id},
				{"url",        j.url},
				{"dest_path",  j.dest_path},
				{"status",     j.status},
				{"progress",   j.progress},
				{"log",        log},
				{"started_at", j.started_at},
			});
		}
		route::ok(res, result.dump());
	});
}
