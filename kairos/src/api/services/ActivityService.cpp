#include "ActivityService.h"
#include "../RouteHelpers.h"
#include "../../log/LogBuffer.h"
#include "../../source/SyncManager.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

ActivityService::ActivityService(const ServiceContext& ctx)
	: sync_(ctx.sync), logs_(ctx.logs) {}

void ActivityService::registerRoutes(httplib::Server& svr) {

	svr.Post("/api/sync/all", [this](const Req&, Res& res) {
		sync_.triggerSync("");
		res.status = 202;
		route::ok(res, json{{"status", "started"}}.dump());
	});

	svr.Get("/api/logs/stream", [this](const Req&, Res& res) {
		res.set_header("Cache-Control",     "no-cache");
		res.set_header("Connection",        "keep-alive");
		res.set_header("X-Accel-Buffering", "no");
		res.set_header("Access-Control-Allow-Origin", "*");

		res.set_chunked_content_provider("text/event-stream",
			[this, cur_seq = uint64_t{0}, sent_init = false]
			(size_t, httplib::DataSink& sink) mutable -> bool {

				if (!sent_init) {
					sent_init = true;
					auto [lines, seq] = logs_.recent(200);
					cur_seq = seq;
					for (const auto& line : lines) {
						std::string ev = "data:" + line + "\n\n";
						if (!sink.write(ev.data(), ev.size())) return false;
					}
					return true;
				}

				auto [new_lines, new_seq] =
					logs_.waitAfter(cur_seq, std::chrono::milliseconds{25'000});

				if (!sink.is_writable()) return false;

				if (new_lines.empty()) {
					static const std::string ping = ": ping\n\n";
					return sink.write(ping.data(), ping.size());
				}

				cur_seq = new_seq;
				for (const auto& line : new_lines) {
					std::string ev = "data:" + line + "\n\n";
					if (!sink.write(ev.data(), ev.size())) return false;
				}
				return true;
			});
	});
}
