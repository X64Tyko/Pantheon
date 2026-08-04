#include "Router.h"
#include "../devices/DeviceRouter.h"
#include "../watchtogether/WatchTogetherRouter.h"
#include "crash/CrashHandler.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>
#include <future>
#include <fstream>
#include <unistd.h>

// MetricsGatherer inline for shared logic across components without complex relative headers in Docker
struct ProcessMetrics
{
	double cpu_usage = 0.0;
	long ram_bytes   = 0;
};

struct SystemMetrics
{
	double total_cpu_usage = 0.0;
	long total_ram_bytes   = 0;
	long free_ram_bytes    = 0;
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

	static SystemMetrics getSystemMetrics()
	{
		SystemMetrics m;
		std::ifstream meminfo("/proc/meminfo");
		std::string label, dummy_kb;
		long value;
		while (meminfo >> label >> value >> dummy_kb)
		{
			if (label == "MemTotal:") m.total_ram_bytes = value * 1024;
			else if (label == "MemAvailable:") m.free_ram_bytes = value * 1024;
		}
		static long last_idle = 0, last_total = 0;
		std::ifstream stat("/proc/stat");
		std::string cpu;
		stat >> cpu;
		long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
		stat >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;
		long total = user + nice + system + idle + iowait + irq + softirq + steal;
		if (last_total > 0)
		{
			long total_delta = total - last_total;
			long idle_delta  = idle - last_idle;
			if (total_delta > 0) m.total_cpu_usage = 100.0 * (1.0 - (double)idle_delta / total_delta);
		}
		last_idle  = idle;
		last_total = total;
		return m;
	}
};

using json = nlohmann::json;

static std::string baseUrl(const httplib::Request& req)
{
	auto host = req.get_header_value("Host");
	if (host.empty()) host = "localhost:8000";
	return "http://" + host;
}

static void handleStream(const std::string& channel_id,
						 BroadcasterManager& broadcasters,
						 httplib::Response& res)
{
	std::cout << "[hermes] stream request: channel=" << channel_id << "\n";

	auto bc   = broadcasters.getOrCreate(channel_id);
	auto sink = bc->addClient();

	res.set_chunked_content_provider(
		"video/mp2t",
		[sink](size_t, httplib::DataSink& data_sink) -> bool
		{
			std::unique_lock<std::mutex> lock(sink->mtx);
			sink->cv.wait(lock, [&]
			{
				return !sink->queue.empty() || sink->done.load();
			});

			if (sink->queue.empty())
			{
				data_sink.done();
				return false;
			}

			auto chunk = std::move(sink->queue.front());
			sink->queue.pop_front();
			lock.unlock();

			return data_sink.write(
				reinterpret_cast<const char*>(chunk.data()),
				chunk.size());
		},
		[sink, bc](bool)
		{
			bc->removeClient(sink);
		}
	);
}

static std::string urlEncodeValue(const std::string& s)
{
	std::string out;
	out.reserve(s.size() * 3);
	static const char* hex = "0123456789ABCDEF";
	for (unsigned char c : s)
	{
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
		{
			out += static_cast<char>(c);
		}
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 0xF];
		}
	}
	return out;
}

// Build query string from httplib params map, re-encoding values.
static std::string buildQuery(const httplib::Params& params)
{
	std::string q;
	for (auto& [k, v] : params) q += (q.empty() ? "" : "&") + k + "=" + urlEncodeValue(v);
	return q;
}

// Downstream services (Kairos) are normally only ever reached through
// Hermes, so without this they have no way to see the real client address —
// every request looks like it's coming from Hermes's own container IP,
// which makes any per-IP abuse throttling downstream (e.g. guest-account
// creation) meaningless. Standard XFF chaining: append this hop's own view
// of the client to whatever's already there (a chain of exactly one entry
// unless something upstream of Hermes — e.g. cloudflared — already set one).
static std::string appendForwardedFor(const httplib::Request& req)
{
	std::string existing = req.get_header_value("X-Forwarded-For");
	return existing.empty() ? req.remote_addr : existing + ", " + req.remote_addr;
}

// Proxy a long-lived streaming response (SSE, chunked) from an upstream service.
// Unlike proxyRequest this never buffers — it pipes bytes to the client as they arrive.
static void proxyStream(const std::string& upstream_base,
						const httplib::Request& req,
						httplib::Response& res)
{
	std::string path = req.path;
	auto q           = buildQuery(req.params);
	if (!q.empty()) path += "?" + q;

	httplib::Headers fwd;
	for (const char* h : {"Authorization", "Cookie", "Accept", "Accept-Language", "X-Pantheon-Surface"})
	{
		auto v = req.get_header_value(h);
		if (!v.empty()) fwd.emplace(h, v);
	}
	fwd.emplace("X-Forwarded-For", appendForwardedFor(req));

	res.set_header("Cache-Control", "no-cache");
	res.set_header("Connection", "keep-alive");
	res.set_header("X-Accel-Buffering", "no");
	res.set_header("Access-Control-Allow-Origin", "*");

	res.set_chunked_content_provider("text/event-stream",
									 [upstream_base, path, fwd](size_t, httplib::DataSink& sink) -> bool
									 {
										 httplib::Client cli(upstream_base);
										 cli.set_connection_timeout(5);
										 cli.set_read_timeout(60);

										 cli.Get(path, fwd,
												 [](const httplib::Response&) -> bool { return true; },
												 [&sink](const char* data, size_t len) -> bool
												 {
													 return sink.is_writable() && sink.write(data, len);
												 });

										 if (sink.is_writable()) sink.done();
										 return false;
									 });
}

// Forward any HTTP request to an upstream service verbatim.
static void proxyRequest(const std::string& upstream_base,
						 const httplib::Request& req,
						 httplib::Response& res)
{
	httplib::Client cli(upstream_base);
	cli.set_connection_timeout(5);
	cli.set_read_timeout(30);

	std::string path = req.path;
	auto q           = buildQuery(req.params);
	if (!q.empty()) path += "?" + q;

	auto ct = req.get_header_value("Content-Type");

	// Forward headers that upstreams need for auth and content negotiation.
	httplib::Headers fwd;
	for (const char* h : {"Authorization", "Cookie", "Accept", "Accept-Language", "X-Pantheon-Surface"})
	{
		auto v = req.get_header_value(h);
		if (!v.empty()) fwd.emplace(h, v);
	}
	fwd.emplace("X-Forwarded-For", appendForwardedFor(req));

	httplib::Result r;
	if (req.method == "GET") r = cli.Get(path, fwd);
	else if (req.method == "POST") r = cli.Post(path, fwd, req.body, ct.c_str());
	else if (req.method == "PUT") r = cli.Put(path, fwd, req.body, ct.c_str());
	else if (req.method == "DELETE") r = cli.Delete(path, fwd, req.body, ct.c_str());
	else if (req.method == "PATCH") r = cli.Patch(path, fwd, req.body, ct.c_str());
	else
	{
		res.status = 405;
		return;
	}

	if (!r || r->status == 0)
	{
		res.status = 502;
		res.set_content(json{{"error", "upstream unavailable"}}.dump(), "application/json");
		return;
	}
	res.status = r->status;
	auto loc   = r->get_header_value("Location");
	if (!loc.empty()) res.set_header("Location", loc);
	// Needed for a Chromecast custom receiver's player pipeline (a JS-level
	// fetch from whatever origin the receiver is hosted at — github.io or a
	// self-hosted domain) to read manifests/segments cross-origin if it ever
	// falls back off native passthrough. Same wide-open posture proxyStream
	// and the SSE log route already use elsewhere in this file.
	res.set_header("Access-Control-Allow-Origin", "*");
	auto resp_ct = r->get_header_value("Content-Type");
	if (!r->body.empty()) res.set_content(r->body, resp_ct.empty() ? "application/octet-stream" : resp_ct);
}

void registerRoutes(httplib::Server& svr, BroadcasterManager& broadcasters,
					KairosClient& kairos, LogBuffer& logs, const Config& cfg,
					DeviceSessionManager& devices, WatchTogetherManager& watch_together)
{
	// ── Health ────────────────────────────────────────────────────────────────
	svr.Get("/health", [](const httplib::Request&, httplib::Response& res)
	{
		res.set_content(
			json{{"status", "ok"}, {"service", "hermes"}}.dump(),
			"application/json");
	});

	// ── Log stream (SSE) — Hermes's own log buffer ───────────────────────────
	svr.Get("/api/logs/stream", [&logs, cfg](const httplib::Request& req, httplib::Response& res)
	{
		// Admin-only, matching Kairos's own /api/logs/stream and the Hades
		// Activity page's own adminOnly nav gate — this was previously wide
		// open to anyone, letting an anonymous caller hold an SSE connection
		// open indefinitely (enough concurrent ones exhaust Hermes's thread
		// pool). Hermes has no session/user concept of its own, so it
		// validates the caller against Kairos's /api/auth/me, same pattern as
		// DELETE /api/activity/crash below. EventSource can't set an
		// Authorization header, so the token may arrive as ?token= instead —
		// check both, same as Kairos's own auth middleware does.
		std::string token = req.get_header_value("Authorization");
		if (token.starts_with("Bearer ")) token = token.substr(7);
		else
		{
			token.clear();
			auto it = req.params.find("token");
			if (it != req.params.end()) token = it->second;
		}
		bool is_admin = false;
		if (!token.empty())
		{
			httplib::Client cli(cfg.kairos_url);
			cli.set_connection_timeout(5);
			cli.set_read_timeout(5);
			if (auto who = cli.Get("/api/auth/me?token=" + urlEncodeValue(token)))
			{
				if (who->status == 200)
				{
					try { is_admin = json::parse(who->body).value("role", "") == "admin"; }
					catch (...)
					{
					}
				}
			}
		}
		if (!is_admin)
		{
			res.status = token.empty() ? 401 : 403;
			res.set_content(json{{"error", token.empty() ? "Unauthorized" : "Forbidden"}}.dump(), "application/json");
			return;
		}

		res.set_header("Cache-Control", "no-cache");
		res.set_header("Connection", "keep-alive");
		res.set_header("X-Accel-Buffering", "no");
		res.set_header("Access-Control-Allow-Origin", "*");

		res.set_chunked_content_provider("text/event-stream",
										 [&logs, cur_seq = uint64_t{0}, sent_init = false]
									 (size_t, httplib::DataSink& sink) mutable -> bool
										 {
											 if (!sent_init)
											 {
												 sent_init         = true;
												 auto [lines, seq] = logs.recent(200);
												 cur_seq           = seq;
												 for (const auto& line : lines)
												 {
													 std::string ev = "data:" + line + "\n\n";
													 if (!sink.write(ev.data(), ev.size())) return false;
												 }
												 return true;
											 }

											 auto [new_lines, new_seq] =
												 logs.waitAfter(cur_seq, std::chrono::milliseconds{25'000});

											 if (!sink.is_writable()) return false;

											 if (new_lines.empty())
											 {
												 static const std::string ping = ": ping\n\n";
												 return sink.write(ping.data(), ping.size());
											 }

											 cur_seq = new_seq;
											 for (const auto& line : new_lines)
											 {
												 std::string ev = "data:" + line + "\n\n";
												 if (!sink.write(ev.data(), ev.size())) return false;
											 }
											 return true;
										 });
	});

	// ── Aggregated Metrics ────────────────────────────────────────────────────
	svr.Get("/api/system/metrics", [cfg](const httplib::Request&, httplib::Response& res)
	{
		auto pm = MetricsGatherer::getProcessMetrics();
		auto sm = MetricsGatherer::getSystemMetrics();

		// Aggregate metrics from Kairos and Hephaestus
		auto fetch_metrics = [](const std::string& url) -> json
		{
			httplib::Client cli(url);
			cli.set_connection_timeout(1);
			cli.set_read_timeout(1);
			if (auto r = cli.Get("/api/system/metrics"))
			{
				if (r->status == 200) return json::parse(r->body);
			}
			if (auto r = cli.Get("/stream/activity/metrics"))
			{
				// Hephaestus
				if (r->status == 200) return json::parse(r->body);
			}
			return json::object();
		};

		auto kairos_f = std::async(std::launch::async, fetch_metrics, cfg.kairos_url);
		auto heph_f   = std::async(std::launch::async, fetch_metrics, cfg.hephaestus_url);

		json j_kairos = kairos_f.get();
		json j_heph   = heph_f.get();

		json out = {
			{
				"hermes", {
					{"cpu_usage", pm.cpu_usage},
					{"ram_bytes", pm.ram_bytes}
				}
			},
			{"kairos", j_kairos},
			{"hephaestus", j_heph},
			{
				"system", {
					{"cpu_usage", sm.total_cpu_usage},
					{"ram_total", sm.total_ram_bytes},
					{"ram_free", sm.free_ram_bytes}
				}
			}
		};

		res.set_header("Access-Control-Allow-Origin", "*");
		res.set_content(out.dump(), "application/json");
	});

	// ── Aggregated crash markers ──────────────────────────────────────────────
	// Local-only, mirrors /api/system/metrics' fan-out pattern — see
	// shared/crash/CrashHandler.h. Every field empty means nothing has
	// crashed since its marker was last cleared/overwritten; nothing here is
	// ever sent anywhere but this response.
	svr.Get("/api/activity/crash", [cfg](const httplib::Request&, httplib::Response& res)
	{
		auto fetch_crash = [](const std::string& url, const char* path) -> std::string
		{
			httplib::Client cli(url);
			cli.set_connection_timeout(1);
			cli.set_read_timeout(1);
			if (auto r = cli.Get(path))
			{
				if (r->status == 200)
				{
					try { return json::parse(r->body).value("crash", ""); }
					catch (...)
					{
					}
				}
			}
			return "";
		};

		auto kairos_f = std::async(std::launch::async, fetch_crash, cfg.kairos_url, "/api/activity/crash");
		auto heph_f   = std::async(std::launch::async, fetch_crash, cfg.hephaestus_url, "/stream/activity/crash");

		json out = {
			{"hermes", readCrashMarker("./data", "hermes")},
			{"kairos", kairos_f.get()},
			{"hephaestus", heph_f.get()},
		};
		res.set_header("Access-Control-Allow-Origin", "*");
		res.set_content(out.dump(), "application/json");
	});

	// Explicit "I've seen this" acknowledgment across all three services at
	// once — admin-gated here (checked against Kairos, same pattern as
	// authedHephaestusProxy) since hermes's own marker-clear below doesn't
	// otherwise pass through Kairos's own admin check the way the proxied
	// ones do. The Authorization header is forwarded on to Kairos'/
	// Hephaestus' own DELETE calls regardless, so their own gating (Kairos
	// is admin-only; Hephaestus has none, matching its GET) still applies
	// independently of this check.
	svr.Delete("/api/activity/crash", [cfg](const httplib::Request& req, httplib::Response& res)
	{
		auto auth = req.get_header_value("Authorization");
		httplib::Result who;
		if (!auth.empty())
		{
			httplib::Client cli(cfg.kairos_url);
			cli.set_connection_timeout(5);
			cli.set_read_timeout(5);
			who = cli.Get("/api/auth/me", httplib::Headers{{"Authorization", auth}});
		}
		bool is_admin = false;
		if (who && who->status == 200)
		{
			try { is_admin = json::parse(who->body).value("role", "") == "admin"; }
			catch (...)
			{
			}
		}
		if (auth.empty() || !who || who->status != 200)
		{
			res.status = 401;
			res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
			return;
		}
		if (!is_admin)
		{
			res.status = 403;
			res.set_content(json{{"error", "Forbidden"}}.dump(), "application/json");
			return;
		}

		auto clear_remote = [&auth](const std::string& url, const char* path) -> bool
		{
			httplib::Client cli(url);
			cli.set_connection_timeout(2);
			cli.set_read_timeout(2);
			if (auto r = cli.Delete(path, httplib::Headers{{"Authorization", auth}}))
			{
				return r->status == 200;
			}
			return false;
		};

		json out = {
			{"hermes", clearCrashMarker("./data", "hermes")},
			{"kairos", clear_remote(cfg.kairos_url, "/api/activity/crash")},
			{"hephaestus", clear_remote(cfg.hephaestus_url, "/stream/activity/crash")},
		};
		res.set_header("Access-Control-Allow-Origin", "*");
		res.set_content(out.dump(), "application/json");
	});

	// ── HDHomeRun device emulation ────────────────────────────────────────────
	svr.Get("/discover.json", [&cfg](const httplib::Request& req, httplib::Response& res)
	{
		auto base = baseUrl(req);
		res.set_content(json{
							{"FriendlyName", cfg.hdhr_friendly},
							{"Manufacturer", "Silicondust"},
							{"ManufacturerURL", "https://github.com/X64Tyko/Pantheon"},
							{"ModelNumber", "HDTC-2US"},
							{"FirmwareName", "hdhomeruntc_atsc"},
							{"TunerCount", cfg.hdhr_tuner_count},
							{"FirmwareVersion", "20170930"},
							{"DeviceID", cfg.hdhr_device_id},
							{"DeviceAuth", ""},
							{"BaseURL", base},
							{"LineupURL", base + "/lineup.json"},
						}.dump(), "application/json");
	});

	svr.Get("/device.xml", [&cfg](const httplib::Request& req, httplib::Response& res)
	{
		auto base       = baseUrl(req);
		std::string xml =
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			"<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
			"  <URLBase>" + base + "</URLBase>\n"
			"  <specVersion><major>1</major><minor>0</minor></specVersion>\n"
			"  <device>\n"
			"    <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>\n"
			"    <friendlyName>" + cfg.hdhr_friendly + "</friendlyName>\n"
			"    <manufacturer>Silicondust</manufacturer>\n"
			"    <modelName>HDTC-2US</modelName>\n"
			"    <modelNumber>HDTC-2US</modelNumber>\n"
			"    <serialNumber/>\n"
			"    <UDN>uuid:" + cfg.hdhr_device_id + "</UDN>\n"
			"  </device>\n"
			"</root>\n";
		res.set_content(xml, "application/xml");
	});

	svr.Get("/lineup_status.json", [](const httplib::Request&, httplib::Response& res)
	{
		res.set_content(json{
							{"ScanInProgress", 0},
							{"ScanPossible", 1},
							{"Source", "Cable"},
							{"SourceList", json::array({"Cable"})},
						}.dump(), "application/json");
	});

	svr.Get("/lineup.json", [&kairos](const httplib::Request& req, httplib::Response& res)
	{
		auto base     = baseUrl(req);
		auto channels = kairos.getChannels();
		json lineup   = json::array();
		for (auto& ch : channels)
		{
			lineup.push_back({
				{"GuideNumber", std::to_string(ch.number)},
				{"GuideName", ch.name},
				{"URL", base + "/stream/channels/" + ch.channel_id + ".ts"},
			});
		}
		res.set_content(lineup.dump(), "application/json");
	});

	// Note: /api/images/proxy is intentionally NOT handled here. It used to have
	// its own naive implementation (no Referer/User-Agent for AniDB hotlink
	// protection, no disk/negative caching, plus an extra network round-trip to
	// Kairos just to re-validate the token). That duplicated and shadowed
	// Kairos's own /api/images/proxy (registered public, no auth required),
	// which already has the correct headers, AniDB rate limiting, and caching.
	// Falling through to the generic Kairos API proxy below gets that for free.

	// ── MPEG-TS live stream ───────────────────────────────────────────────────
	// DVR clients (Plex, Jellyfin) send Range headers even for live streams.
	// httplib's range_error check fires after the handler and returns 416
	// whenever the chunked content provider has no content-length (always for
	// live streams). The Request object is non-const internally — clearing
	// ranges here prevents the spurious 416 while keeping chunked encoding.
	svr.Get(R"(/stream/channels/([^/.]+)$)", [&broadcasters](
			const httplib::Request& req, httplib::Response& res)
			{
				const_cast<httplib::Request&>(req).ranges.clear();
				handleStream(req.matches[1], broadcasters, res);
			});

	svr.Get(R"(/stream/channels/([^/]+)\.ts$)", [&broadcasters](
			const httplib::Request& req, httplib::Response& res)
			{
				const_cast<httplib::Request&>(req).ranges.clear();
				handleStream(req.matches[1], broadcasters, res);
			});

	// ── HLS / VOD proxy — 1:1 pass-through to Hephaestus, no fan-out ──────────
	// Unlike /stream/channels/:id (MPEG-TS, shared ClientSink fan-out via
	// handleStream above), HLS manifests/segments/subtitles are finite,
	// complete HTTP responses that Hephaestus already serves as static
	// files — proxyRequest's buffered pass-through is the right fit, same as
	// the Kairos API proxy below. Registered before the Hades catch-all.
	auto hephaestusProxy = [&cfg](const httplib::Request& req, httplib::Response& res)
	{
		proxyRequest(cfg.hephaestus_url, req, res);
	};
	svr.Get(R"(/stream/hls/channels/.*)", hephaestusProxy);
	// Capability-bucketed per-viewer channel HLS (see Hephaestus's
	// ChannelViewerRegistry) — plain pass-through same as the legacy channel
	// HLS route above; all viewer-identity/bucket-resolution logic lives on
	// the Hephaestus side, nothing here needs to be session-aware.
	svr.Get(R"(/stream/hls/channel-viewer/.*)", hephaestusProxy);
	svr.Get(R"(/stream/vod/.*)", hephaestusProxy);
	svr.Get(R"(/stream/preview/.*)", hephaestusProxy);

	// POST /stream/vod|preview/... (start/switch/stop) is the one place on
	// this router that would otherwise let anyone on the network stream the
	// private library with no login — unlike /stream/channels/:id (open on
	// purpose, for HDHomeRun/DVR compatibility) there's no third-party-client
	// reason for this to be unauthenticated. Hermes has no session store of
	// its own, so it asks Kairos to validate the caller's token first.
	auto authedHephaestusProxy = [&cfg](const httplib::Request& req, httplib::Response& res)
	{
		auto auth = req.get_header_value("Authorization");
		httplib::Result r;
		if (!auth.empty())
		{
			httplib::Client cli(cfg.kairos_url);
			cli.set_connection_timeout(5);
			cli.set_read_timeout(5);
			r = cli.Get("/api/auth/me", httplib::Headers{{"Authorization", auth}});
		}
		if (auth.empty() || !r || r->status != 200)
		{
			res.status = 401;
			res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
			return;
		}

		// Parental controls — only "start"/"switch" calls carry enough info
		// (content_type+content_id for VOD, channel_id for preview) to check;
		// stop/activity calls operate on an already-authorized session and
		// are left alone. This is the one already-authenticated playback
		// boundary for VOD/preview, so restriction is enforced here as a
		// real security check, not just hidden from browse/list views.
		// Fails open on an infrastructure error (Kairos already had to be
		// reachable for the auth check above to have succeeded at all) and
		// closed only on an explicit "not allowed" response.
		bool is_start_or_switch = req.path.ends_with("/start") || req.path.ends_with("/switch");
		if (is_start_or_switch)
		{
			try
			{
				auto body = json::parse(req.body);
				httplib::Client kairos(cfg.kairos_url);
				kairos.set_connection_timeout(5);
				kairos.set_read_timeout(5);
				httplib::Result check;
				if (req.path.starts_with("/stream/vod/") &&
					body.contains("content_type") && body.contains("content_id"))
				{
					std::string path = "/api/content/" + body["content_type"].get<std::string>()
						+ "/" + body["content_id"].get<std::string>() + "/access-check";
					check = kairos.Get(path, httplib::Headers{{"Authorization", auth}});
				}
				else if (req.path.starts_with("/stream/preview/") && body.contains("channel_id"))
				{
					std::string path = "/api/channels/" + body["channel_id"].get<std::string>() + "/access-check";
					check            = kairos.Get(path, httplib::Headers{{"Authorization", auth}});
				}
				if (check && check->status == 200)
				{
					auto cj = json::parse(check->body);
					if (!cj.value("allowed", true))
					{
						res.status = 403;
						res.set_content(json{{"error", "Restricted"}}.dump(), "application/json");
						return;
					}
				}
			}
			catch (...)
			{
				/* malformed body or unreachable check — fail open, per above */
			}
		}

		proxyRequest(cfg.hephaestus_url, req, res);
	};
	svr.Post(R"(/stream/vod/.*)", authedHephaestusProxy);
	svr.Post(R"(/stream/preview/.*)", authedHephaestusProxy);
	// POST /stream/channel/:id/start and /stream/channel/viewer/:id/stop —
	// same "no anonymous access to the private library" reasoning as VOD/
	// preview above. Neither path matches the vod/preview access-check
	// branches inside authedHephaestusProxy, so this only ever enforces the
	// Bearer-token-valid check, not a per-content restriction — channels are
	// schedule-driven broadcast, not a per-item selection the way VOD/
	// preview are, so there's nothing else to check here.
	svr.Post(R"(/stream/channel/.*)", authedHephaestusProxy);

	// Client-declared decode capability (see ClientCapabilitiesRouter.cpp
	// on the Hephaestus side, which validates the bearer token itself) —
	// no content is referenced here, so the plain pass-through is enough,
	// same as the GET routes above. This was missing entirely: clients'
	// declare/forget calls were falling through to the Hades catch-all
	// below with no error surfaced, so direct-stream silently never had a
	// capability declaration to check against.
	svr.Post(R"(/stream/client-capabilities)", hephaestusProxy);
	svr.Delete(R"(/stream/client-capabilities)", hephaestusProxy);

	// GET /stream/activity/... (Hades' Activity page "Now Playing" panel) —
	// authenticated for the same reason as the POST routes above: unlike a
	// segment/manifest fetch for a stream ID the caller already knows, this
	// exposes what *other* users are currently watching plus internal file
	// paths and server log lines, which is a step beyond what an
	// unauthenticated GET route should hand out.
	svr.Get(R"(/stream/activity/.*)", authedHephaestusProxy);

	// ── M3U playlist ──────────────────────────────────────────────────────────
	svr.Get("/playlist.m3u", [&kairos](const httplib::Request& req, httplib::Response& res)
	{
		auto base       = baseUrl(req);
		auto channels   = kairos.getChannels();
		std::string m3u = "#EXTM3U\n";
		for (auto& ch : channels)
		{
			m3u += "#EXTINF:-1"
				" tvg-id=\"" + ch.channel_id + "\""
				" tvg-name=\"" + ch.name + "\""
				" channel-number=\"" + std::to_string(ch.number) + "\""
				"," + ch.name + "\n"
				+ base + "/stream/channels/" + ch.channel_id + "\n";
		}
		res.set_content(m3u, "application/x-mpegurl");
	});

	// ── M3U / XMLTV aliases ───────────────────────────────────────────────────
	// Common alternate paths used by DVR clients — must come before /api/.*
	// Uses a dedicated client with a longer read timeout than proxyRequest because
	// XMLTV generation can be slow on a cold cache (ensureScheduled for every channel).
	auto xmltvAlias = [&cfg](const httplib::Request& req, httplib::Response& res)
	{
		int hours = 24;
		auto it   = req.params.find("hours");
		if (it != req.params.end())
		{
			try { hours = std::stoi(it->second); }
			catch (...)
			{
			}
		}
		httplib::Client cli(cfg.kairos_url);
		cli.set_connection_timeout(5);
		cli.set_read_timeout(60);
		auto r = cli.Get("/epg.xml?hours=" + std::to_string(hours));
		if (!r || r->status == 0)
		{
			res.status = 502;
			return;
		}
		res.status = r->status;
		res.set_content(r->body, "application/xml");
	};
	svr.Get("/epg.xml", xmltvAlias);
	svr.Get("/api/epg.xml", xmltvAlias);
	svr.Get("/api/xmltv.xml", xmltvAlias);

	svr.Get("/api/channels.m3u", [&kairos](const httplib::Request& req, httplib::Response& res)
	{
		auto base       = baseUrl(req);
		auto channels   = kairos.getChannels();
		std::string m3u = "#EXTM3U\n";
		for (auto& ch : channels)
		{
			m3u += "#EXTINF:-1"
				" tvg-id=\"" + ch.channel_id + "\""
				" tvg-name=\"" + ch.name + "\""
				" channel-number=\"" + std::to_string(ch.number) + "\""
				"," + ch.name + "\n"
				+ base + "/stream/channels/" + ch.channel_id + "\n";
		}
		res.set_content(m3u, "application/x-mpegurl");
	});
	svr.Get("/api/channels.xml", [&cfg](const httplib::Request& req, httplib::Response& res)
	{
		int hours = 24;
		auto it   = req.params.find("hours");
		if (it != req.params.end())
		{
			try { hours = std::stoi(it->second); }
			catch (...)
			{
			}
		}
		httplib::Client cli(cfg.kairos_url);
		cli.set_connection_timeout(5);
		cli.set_read_timeout(30);
		auto r = cli.Get("/epg.xml?hours=" + std::to_string(hours));
		if (!r || r->status == 0)
		{
			res.status = 502;
			return;
		}
		res.status = r->status;
		res.set_content(r->body, "application/xml");
	});

	// ── Roku device sessions ──────────────────────────────────────────────────
	// Registered before the Kairos API proxy below so /api/devices/* is
	// handled here in Hermes (it owns the live, in-memory session state) and
	// never falls through to the Kairos catch-all proxy.
	registerDeviceRoutes(svr, devices, cfg);

	// ── Watch Together live coordination ──────────────────────────────────────
	// Deliberately NOT under /api/watch-together/* — that whole prefix
	// (create/list/join/leave/close) is Kairos-owned persistence/discovery
	// and already reaches Kairos untouched via the generic proxy below.
	// These three routes are the live, in-memory half only (see
	// WatchTogetherSession's class comment) — same /stream/-style path
	// convention as the VOD/preview routes above, for the same reason: it
	// needs to sit outside the /api/ namespace the Kairos proxy owns wholesale.
	registerWatchTogetherRoutes(svr, watch_together, cfg);

	// ── Kairos API proxy (all methods) ────────────────────────────────────────
	// Registered before the Hades catch-all so /api/* routes never reach nginx.
	auto kairosProxy = [&cfg](const httplib::Request& req, httplib::Response& res)
	{
		proxyRequest(cfg.kairos_url, req, res);
	};
	svr.Get(R"(/api/.*)", kairosProxy);
	svr.Post(R"(/api/.*)", kairosProxy);
	svr.Put(R"(/api/.*)", kairosProxy);
	svr.Delete(R"(/api/.*)", kairosProxy);
	svr.Patch(R"(/api/.*)", kairosProxy);

	// ── Hades UI catch-all ────────────────────────────────────────────────────
	// Everything that didn't match above is the SPA. nginx's try_files handles
	// SPA routing server-side so deep links and refreshes work.
	auto hadesProxy = [&cfg](const httplib::Request& req, httplib::Response& res)
	{
		proxyRequest(cfg.hades_url, req, res);
	};
	svr.Get(".*", hadesProxy);
	svr.Post(".*", hadesProxy);
}