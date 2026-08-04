#include "ActivityRouter.h"
#include "crash/CrashHandler.h"
#include "log/LogBuffer.h"
#include "../stream/ChannelViewerRegistry.h"
#include "../stream/EncoderArgs.h" // hwAccelName
#include "../stream/GpuMetrics.h"
#include "../stream/SessionManager.h"
#include "../stream/VodSessionManager.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <fstream>
#include <unistd.h>

// MetricsGatherer inline for shared logic across components without complex relative headers in Docker
struct ProcessMetrics
{
	double cpu_usage = 0.0;
	long ram_bytes   = 0;
};

class MetricsGatherer
{
public:
	static ProcessMetrics getProcessMetrics()
	{
		ProcessMetrics m;

		// RAM: prefer cgroup memory for container-accurate reporting.
		// Docker reports cgroup memory, not per-process RSS.
		// Try cgroup v2, then v1, then fall back to /proc/self/statm RSS.
		bool ram_set = false;
		for (const char* path : {
				 "/sys/fs/cgroup/memory.current", // cgroup v2
				 "/sys/fs/cgroup/memory/memory.usage_in_bytes"
			 }) // cgroup v1
		{
			std::ifstream f(path);
			long val;
			if (f >> val)
			{
				m.ram_bytes = val;
				ram_set     = true;
				break;
			}
		}
		if (!ram_set)
		{
			std::ifstream statm("/proc/self/statm");
			long dummy, rss;
			if (statm >> dummy >> rss) m.ram_bytes = rss * sysconf(_SC_PAGESIZE);
		}

		// CPU: expressed as a fraction of one core (100% = one full core),
		// matching Docker's scale. /proc/stat totals span all host CPUs, so
		// multiply proc_delta by nproc to normalize to per-core percentage.
		static long last_utime = 0, last_stime = 0, last_total_time = 0;
		std::ifstream stat("/proc/self/stat");
		std::string dummy;
		for (int i = 0; i < 13; ++i) stat >> dummy;
		long utime, stime;
		stat >> utime >> stime;
		std::ifstream uptime("/proc/stat");
		std::string cpu;
		uptime >> cpu;
		long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
		uptime >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;
		long total_time = user + nice + system + idle + iowait + irq + softirq + steal;
		if (last_total_time > 0)
		{
			long total_delta = total_time - last_total_time;
			long proc_delta  = (utime + stime) - (last_utime + last_stime);
			if (total_delta > 0)
			{
				int nproc   = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
				m.cpu_usage = 100.0 * proc_delta * nproc / total_delta;
			}
		}
		last_utime      = utime;
		last_stime      = stime;
		last_total_time = total_time;
		return m;
	}
};

using json = nlohmann::json;

namespace
{
	// VodSession has no human title (Router.cpp's /stream/vod/start handler
	// fetches one from Kairos but never threads it into VodSession — adding
	// that would mean changing VodSession::start()'s signature, which Router.cpp
	// calls positionally, so it isn't a change confined to this router). The
	// filename stem is a reasonable stand-in for a debugging/activity view.
	std::string titleFromPath(const std::string& file_path)
	{
		if (file_path.empty()) return "";
		return std::filesystem::path(file_path).stem().string();
	}

	// bucketed_hls_counts: channel_id -> bucket -> exact viewer count, from
	// ChannelViewerRegistry::viewerCounts() — the one source of a real count
	// for HLS viewers (client_count is exact but MPEG-TS/DVR-only; plain HLS
	// otherwise only ever has hls_viewer_active's presence signal, no
	// per-viewer identity to count against at all).
	json channelSessionJson(const std::shared_ptr<ChannelSession>& s,
							const std::map<std::string, std::map<std::string, int>>& bucketed_hls_counts)
	{
		int hls_viewer_count = 0;
		if (auto ch = bucketed_hls_counts.find(s->channelId()); ch != bucketed_hls_counts.end())
		{
			if (auto b = ch->second.find(s->bucketName()); b != ch->second.end()) hls_viewer_count = b->second;
		}
		return {
			{"id", s->channelId()},
			{"kind", "channel"},
			{"title", s->currentTitle()},
			{"file_path", s->currentFilePath()},
			{"hw_accel", hwAccelName(s->hwAccel())},
			{"decode_hw_accel", hwAccelName(s->decodeHwAccel())},
			{"started_at_ms", s->sessionStartMs()},
			// bucket distinguishes this row when a channel has both a
			// "default" (transcode) and "native" (direct-stream) session
			// active at once — previously indistinguishable in this listing.
			{"bucket", s->bucketName()},
			// client_count is exact (native MPEG-TS/DVR clients). hls_viewer_active
			// stays as a presence-only fallback for viewers on the legacy,
			// non-bucketed HLS URL (no per-viewer identity there at all);
			// hls_viewer_count is the real number for viewers who came through
			// the capability-bucketed opt-in path (ChannelViewerRegistry) —
			// together these replace the old "always shows >=1" HLS story.
			{"client_count", s->clientCount()},
			{"hls_viewer_active", s->hlsViewerActive()},
			{"hls_viewer_count", hls_viewer_count},
		};
	}

	json vodSessionJson(const std::shared_ptr<VodSession>& s)
	{
		return {
			{"id", s->sessionId()},
			{"kind", "vod"},
			{"title", titleFromPath(s->filePath())},
			{"file_path", s->filePath()},
			{"hw_accel", hwAccelName(s->hwAccel())},
			{"decode_hw_accel", hwAccelName(s->decodeHwAccel())},
			{"started_at_ms", s->startedAtMs()},
			{"direct_stream", s->directStream()},
			// Sliding-window lookahead engine state — lets the debug panel
			// confirm the encoder is actually pausing/resuming/restarting as
			// expected rather than needing to shell into the box.
			{"duration_ms", s->durationMs()},
			{"total_segments", s->totalSegments()},
			{"highest_generated_segment", s->highestGeneratedSegment()},
			{"last_requested_segment", s->lastRequestedSegment()},
			{"main_encoder_paused", s->isMainEncoderPaused()},
		};
	}
} // namespace

void registerActivityRoutes(httplib::Server& svr, SessionManager& sessions,
							VodSessionManager& vodSessions, LogBuffer& logs,
							HwAccel gpu_backend, const std::string& vaapi_device,
							ChannelViewerRegistry& channelViewers)
{
	svr.Get("/stream/activity/sessions", [&sessions, &vodSessions, &channelViewers](
			const httplib::Request&, httplib::Response& res)
			{
				auto bucketed_hls_counts = channelViewers.viewerCounts();
				json out                 = json::array();
				for (auto& s : sessions.listActive())
				{
					// A ChannelSession can be running (ffmpeg spawned, warm) with
					// genuinely nobody attached to it — e.g. the default bucket,
					// briefly created just to resolve a cold channel's current
					// item/audio track for a viewer who then actually landed on
					// the native bucket instead (see Router.cpp's
					// /stream/channel/:id/start). Skipping these keeps this
					// listing to real "someone is watching this" channels, not
					// every warm-but-unwatched encoder instance.
					if (s->clientCount() == 0 && !s->hlsViewerActive()) continue;
					out.push_back(channelSessionJson(s, bucketed_hls_counts));
				}
				for (auto& s : vodSessions.listActive()) out.push_back(vodSessionJson(s));
				res.set_content(out.dump(), "application/json");
			});

	// v1 approach: the shared LogBuffer isn't partitioned per session, so
	// this filters the recent-lines window by the "[session:<id>"/
	// "[vod:<id>"/"[preview:<id>" prefix every session's own logging already
	// uses (see ChannelSession/VodSession/PreviewSession's std::cerr/cout
	// calls) rather than a live per-session SSE stream — Hermes's proxyRequest
	// fully buffers the response before forwarding it, so it can't relay an
	// SSE stream anyway (a stream that never completes would just hang the
	// proxy). Polling this endpoint is the pattern that actually works
	// through that proxy.
	svr.Get(R"(/stream/activity/sessions/([^/]+)/logs)", [&logs](
			const httplib::Request& req, httplib::Response& res)
			{
				std::string id = req.matches[1];
				int lines      = 500;
				if (req.has_param("lines"))
				{
					try { lines = std::stoi(req.get_param_value("lines")); }
					catch (...)
					{
					}
				}

				const std::string tagChannel = "[session:" + id;
				const std::string tagVod     = "[vod:" + id;
				const std::string tagPreview = "[preview:" + id;

				// Pull a generously large recent window (the shared buffer holds up
				// to LogBuffer::kMax=2000 lines total across every session) then
				// filter down to this session's own lines.
				auto [recent, seq] = logs.recent(LogBuffer::kMax);
				std::vector<std::string> matched;
				for (auto& line : recent)
				{
					if (line.find(tagChannel) != std::string::npos ||
						line.find(tagVod) != std::string::npos ||
						line.find(tagPreview) != std::string::npos)
						matched.push_back(line);
				}
				if (static_cast<int>(matched.size()) > lines) matched.erase(matched.begin(), matched.end() - lines);

				res.set_content(json(matched).dump(), "application/json");
			});

	svr.Get("/stream/activity/metrics", [gpu_backend, vaapi_device](const httplib::Request&, httplib::Response& res)
	{
		auto pm = MetricsGatherer::getProcessMetrics();
		json j  = {
			{"cpu_usage", pm.cpu_usage},
			{"ram_bytes", pm.ram_bytes}
		};
		// Absent entirely when gpu_backend == HwAccel::none or the query
		// itself fails — the Activity page draws nothing in either case
		// rather than an empty/zeroed GPU section (real feedback: "not
		// drawn at all" when no hw-accel is available, not a blank card).
		if (auto gpu = queryGpuMetrics(gpu_backend, vaapi_device))
		{
			json j_gpu = {
				{"backend", gpu->backend},
				{"gpu_util_pct", gpu->gpu_util_pct},
				{"mem_used_mb", gpu->mem_used_mb},
				{"mem_total_mb", gpu->mem_total_mb},
			};
			if (gpu->name) j_gpu["name"] = *gpu->name;
			if (gpu->encoder_util_pct) j_gpu["encoder_util_pct"] = *gpu->encoder_util_pct;
			if (gpu->decoder_util_pct) j_gpu["decoder_util_pct"] = *gpu->decoder_util_pct;
			if (gpu->temp_c) j_gpu["temp_c"] = *gpu->temp_c;
			j["gpu"] = j_gpu;
		}
		res.set_content(j.dump(), "application/json");
	});

	// Local-only crash marker — see shared/crash/CrashHandler.h. Empty
	// string means no crash recorded. Never sent anywhere but this response.
	svr.Get("/stream/activity/crash", [](const httplib::Request&, httplib::Response& res)
	{
		json j = {{"crash", readCrashMarker("./data", "hephaestus")}};
		res.set_content(j.dump(), "application/json");
	});

	// Explicit "I've seen this" acknowledgment — no auth check here (matches
	// the rest of this router; Hermes' aggregated DELETE is the one that
	// actually gates this on admin, same split as the GET above).
	svr.Delete("/stream/activity/crash", [](const httplib::Request&, httplib::Response& res)
	{
		json j = {{"ok", clearCrashMarker("./data", "hephaestus")}};
		res.set_content(j.dump(), "application/json");
	});
}