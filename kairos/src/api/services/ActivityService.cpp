#include "ActivityService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "crash/CrashHandler.h"
#include "log/LogBuffer.h"
#include "../../db/PlaybackHistoryRepository.h"
#include "../../source/SyncManager.h"
#include "../../util/MetricsGatherer.h"
#include "metrics/OperationMetrics.h"
#include <nlohmann/json.hpp>
#include <algorithm>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

ActivityService::ActivityService(const ServiceContext& ctx)
	: db_(ctx.db), sync_(ctx.sync), logs_(ctx.logs) {}

void ActivityService::registerRoutes(httplib::Server& svr) {

	svr.Post("/api/sync/all", [this](const Req&, Res& res) {
		sync_.triggerSync("");
		res.status = 202;
		route::ok(res, json{{"status", "started"}}.dump());
	});

	// Wipes source_mapping for every source first, so the whole library is
	// re-resolved from scratch — same as triggering a Hard Sync per source,
	// just all at once. Admin-gated, unlike the plain sync-all above, since
	// it also discards any manual cross-source links system-wide.
	svr.Post("/api/sync/all-hard", [this](const Req&, Res& res) {
		if (!currentUser() || currentUser()->role != "admin") { route::err(res, 403, "Forbidden"); return; }
		sync_.triggerHardSync("");
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

	svr.Get("/api/system/metrics", [](const Req&, Res& res) {
		auto pm = MetricsGatherer::getProcessMetrics();
		auto sm = MetricsGatherer::getSystemMetrics();
		json j = {
			{"cpu_usage", pm.cpu_usage},
			{"ram_bytes", pm.ram_bytes},
			{"system", {
				{"cpu_usage", sm.total_cpu_usage},
				{"ram_total", sm.total_ram_bytes},
				{"ram_free",  sm.free_ram_bytes}
			}}
		};
		route::ok(res, j.dump());
	});

	// Structured per-operation timing/CPU/RAM/thread stats for the hot zones
	// (full sync + its phases, EPG regeneration, scraper matching, chapter
	// sync) — see shared/metrics/OperationMetrics.h. Distinct from the
	// whole-process point-in-time gauge above: this is a bounded history of
	// completed runs, most-recent-first, keyed by operation name.
	svr.Get("/api/metrics/operations", [](const Req&, Res& res) {
		json out = json::object();
		for (const auto& [name, runs] : OperationMetricsStore::global().snapshot()) {
			json arr = json::array();
			for (const auto& r : runs) {
				arr.push_back({
					{"started_at_ms",  r.started_at_ms},
					{"duration_ms",    r.duration_ms},
					{"avg_cpu_pct",    r.avg_cpu_pct},
					{"max_cpu_pct",    r.max_cpu_pct},
					{"avg_ram_bytes",  r.avg_ram_bytes},
					{"max_ram_bytes",  r.max_ram_bytes},
					{"peak_threads",   r.peak_threads},
					{"samples",        r.samples},
				});
			}
			out[name] = arr;
		}
		route::ok(res, out.dump());
	});

	// Local-only crash marker — see shared/crash/CrashHandler.h. Empty string
	// means no crash recorded since the marker file was last overwritten (or
	// ever). Never sent anywhere but this response; purely for the admin's
	// own Activity/Debugging view.
	svr.Get("/api/activity/crash", [](const Req&, Res& res) {
		route::ok(res, json{{"crash", readCrashMarker("./data", "kairos")}}.dump());
	});

	// Play history for the Tautulli-style Activity tab — see
	// PlaybackHistoryRepository / migration v91. A non-admin viewer only
	// ever sees their own history regardless of the user_id query param
	// (silently overridden, not a 403 — this endpoint is a personal history
	// view first, an admin-wide one second); an admin can pass user_id to
	// filter to one person, or omit it to see everyone.
	svr.Get("/api/activity/history", [this](const Req& req, Res& res) {
		auto user = currentUser();
		if (!user) { route::err(res, 401, "Unauthorized"); return; }

		std::string user_filter = req.has_param("user_id") ? req.get_param_value("user_id") : "";
		if (user->role != "admin") user_filter = user->user_id;

		int64_t from_ms = 0, to_ms = 0;
		int limit = 200;
		try {
			if (req.has_param("from"))  from_ms = std::stoll(req.get_param_value("from"));
			if (req.has_param("to"))    to_ms   = std::stoll(req.get_param_value("to"));
			if (req.has_param("limit")) limit   = std::clamp(std::stoi(req.get_param_value("limit")), 1, 1000);
		} catch (...) { route::err(res, 400, "invalid from/to/limit"); return; }

		json out = json::array();
		for (auto& r : PlaybackHistoryRepository(db_).list(user_filter, from_ms, to_ms, limit)) {
			out.push_back({
				{"event_id",            r.event_id},
				{"user_id",             r.user_id},
				{"content_type",        r.content_type},
				{"content_id",          r.content_id},
				{"title",               r.title},
				{"device_type",         r.device_type},
				{"direct_play",         r.direct_play},
				{"started_at_ms",       r.started_at_ms},
				{"ended_at_ms",         r.ended_at_ms},
				{"started_position_ms", r.started_position_ms},
				{"last_position_ms",    r.last_position_ms},
				{"duration_ms",         r.duration_ms},
				{"completed",           r.completed},
			});
		}
		route::ok(res, out.dump());
	});
}
