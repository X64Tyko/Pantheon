#include "Router.h"
#include "ClientCapabilitiesRouter.h" // extractBearerToken
#include "../kairos/InternalToken.h"
#include "../stream/MediaProbe.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using json = nlohmann::json;

// Polls briefly for a file ffmpeg is expected to produce shortly after a
// session starts (first HLS segment/playlist write). Avoids serving a 503 on
// the very first request just because ffmpeg hasn't flushed its first
// segment yet.
//
// 10s, not a smaller "should be plenty" number: cold start on a live channel
// stacks a probeMedia() ffprobe call, process spawn/codec init, and (since
// the live encode is -re-paced) a real wall-clock wait for the first
// hls_time-second segment to close — measured routinely landing at 4-5s even
// with hls_time=2. VOD adds its own probe-before-spawn cost too. A too-tight
// budget here doesn't fail gracefully — the client (hls.js) sees a 503,
// burns its own retry budget, and surfaces a fatal "stream stopped
// responding" with nothing useful in the server logs, since nothing here
// actually errored.
static bool waitForFile(const std::string& path, int maxWaitMs = 10000)
{
	for (int waited = 0; waited < maxWaitMs; waited += 100)
	{
		if (std::filesystem::exists(path)) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return std::filesystem::exists(path);
}

// subs.vtt is written by its own small, independent, whole-file ffmpeg
// process (see VodSession's class comment) — decoupled from the main
// encoder's own pause/restart cycle, but NOT decoupled from the source
// file's size the way an earlier version of this comment assumed. For an
// EMBEDDED track (buildVodSubtitleArgs' `-map 0:s:N` branch, no external
// sidecar `-i`), this is the file's only mapped input and there's no `-ss`
// (it has to cover the whole runtime, since the sliding-window main encoder
// can seek anywhere over the session's life) — ffmpeg's demuxer therefore
// still has to sequentially read every interleaved video/audio packet in
// the ENTIRE container to reach each subtitle packet, exactly as slow as a
// full-file direct-stream remux was, just without an encode on top. A real
// movie/episode file (many GB, especially over network storage) can easily
// take much longer than a few seconds for that — nothing here is actually
// "small" for that case. An EXTERNAL sidecar (the `-map 1:s:0` branch) is
// genuinely fast regardless of the source file's size: the video file is
// still passed as input 0 so ffmpeg can probe it, but with no `-map`
// referencing it, ffmpeg's demux loop never has anywhere to send its
// packets and doesn't bother reading through it — only the tiny sidecar
// file (input 1) actually gets demuxed. That asymmetry is exactly why
// external subtitles kept working through this rearchitecture while
// embedded ones stopped: this wait used to be bounded by the old
// single-process design's OWN generous 120s budget (waitForFfmpegExit) and
// was rarely hit in practice; shrinking it to look "safely" under
// ExoPlayer's 8s client timeout also shrank it well below what a real
// embedded extraction on a real file needs. Restored to the old budget —
// blocking one shared worker thread for up to this long is the same
// trade-off the old code already made.
static bool waitForSubtitleExtraction(const VodSession& session, int maxWaitMs = 120000)
{
	for (int waited = 0; waited < maxWaitMs; waited += 200)
	{
		if (session.hasSubtitleExtractionExited()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	return session.hasSubtitleExtractionExited();
}

// Reads a whole file into memory. Only called on a segment-cache miss when
// caching is enabled — the common (disabled) case never pays this cost, it
// stays on set_file_content's own streamed disk read below.
static bool readFileBytes(const std::string& path, std::string& out)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return false;
	std::streamsize size = f.tellg();
	if (size < 0) return false;
	f.seekg(0, std::ios::beg);
	out.resize(static_cast<size_t>(size));
	if (size > 0 && !f.read(out.data(), size)) return false;
	return true;
}

// group_key/group_cap: only meaningful for segments (ignored for manifests)
// — see SegmentCache::put's own comment. Passing group_cap=0 (the default)
// just means "no per-group ceiling, rely on the global byte budget alone",
// which is what every non-live-channel call site below wants.
static void serveHlsFile(const std::string& path, const std::string& content_type,
						 httplib::Response& res, SegmentCache& cache,
						 const std::string& group_key = "", size_t group_cap = 0)
{
	// The manifest is rewritten every few seconds for the life of the session
	// (a live channel's canonical playlist.m3u8 via ChannelPlaylistSplicer, or
	// a VOD/audio playlist while its VodEncodeStream is still generating) —
	// with no explicit Cache-Control at all, an intermediary in the real
	// deployment path (Cloudflare Tunnel, a browser's own HTTP cache) is free
	// to cache a snapshot and keep serving it, which reads to hls.js as the
	// segment list randomly jumping backward/forward between whatever
	// snapshot each individual request happened to get — a plausible
	// explanation for reports of playback "rewinding, then snapping back and
	// forth." Segment (.ts) files are the opposite: each one is written
	// exactly once under a filename that's never reused (splicer's own
	// monotonic next_seq_, or a VOD/live producer's own never-reused segment
	// index) and never modified afterward, so they're safe — actively good,
	// even — to let any intermediary (or our own in-memory cache below) cache
	// aggressively and indefinitely.
	bool is_manifest = content_type == "application/vnd.apple.mpegurl";

	if (!is_manifest)
	{
		if (auto cached = cache.get(path))
		{
			res.set_header("Cache-Control", "public, max-age=31536000, immutable");
			res.set_content(*cached, content_type);
			return;
		}
	}

	if (!std::filesystem::exists(path))
	{
		res.status = 404;
		res.set_content(json{{"error", "not found"}}.dump(), "application/json");
		return;
	}

	res.set_header("Cache-Control", is_manifest
										? "no-store, no-cache, must-revalidate"
										: "public, max-age=31536000, immutable");
	if (is_manifest) res.set_header("Pragma", "no-cache");

	if (is_manifest || !cache.enabled())
	{
		res.set_file_content(path, content_type);
		return;
	}

	std::string bytes;
	if (!readFileBytes(path, bytes))
	{
		// Existed a moment ago (checked above) but failed to read fully —
		// rare (e.g. deleted mid-read); fall back to the normal path instead
		// of caching/serving a partial body.
		res.set_file_content(path, content_type);
		return;
	}
	cache.put(path, bytes, group_key, group_cap);
	res.set_content(bytes, content_type);
}

// The channel-viewer playlist route hands out a fresh viewer_session_id-
// scoped manifest URL per viewer, but the segment lines inside it are bare
// relative filenames (straight from ffmpeg's hls muxer) — hls.js resolves
// those relative to whatever URL it fetched the manifest from, so two
// viewers pinned to the identical (channel_id, bucket) end up requesting
// their *own*, distinct segment URLs for the exact same bytes. Hermes can
// only cache/coalesce across viewers if it sees the same request path for
// the same content, so this rewrites each bare "seg-NNNNN.ts" line into the
// absolute, content-addressed form the bucket-explicit legacy-style route
// below serves — same bytes, but now one URL shared by every viewer of this
// channel+bucket regardless of which viewer_session_id fetched the
// manifest. Plain text substitution, not a full M3U8 parser: every non-'#'
// line ffmpeg's hls muxer ever writes is a segment filename. Absolute-path
// URIs are valid HLS (RFC 8216) and standard practice for CDN manifest
// rewriting.
//
// instance_id (ChannelSession::instanceId()) is embedded as an opaque path
// component purely so the URL itself changes if this (channel_id, bucket)
// session ever gets torn down and replaced (SessionManager::getOrCreate()
// does this silently, resetting segment numbering back to seg-00000.ts on
// the same hlsDir()) — Hermes's cache keys on request path, has no other
// way to know a "new" seg-00003.ts isn't the same content as the old
// seg-00003.ts it already cached under the same URL. The route below never
// inspects this value, only consumes it — the current live session is
// always what actually gets served either way.
static std::string rewriteChannelViewerPlaylist(const std::string& content, const std::string& channel_id,
												const std::string& bucket, int64_t instance_id)
{
	std::string prefix =
		"/stream/hls/channels/" + channel_id + "/" + bucket + "/" + std::to_string(instance_id) + "/";
	std::string out;
	out.reserve(content.size() + 32);
	size_t pos = 0;
	while (pos <= content.size())
	{
		size_t nl        = content.find('\n', pos);
		std::string line = (nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
		if (!line.empty() && line[0] != '#') out += prefix;
		out += line;
		if (nl == std::string::npos) break;
		out += '\n';
		pos = nl + 1;
	}
	return out;
}

// Unconditionally overwrites a per-viewer "last served" snapshot (raw disk
// content + what actually got sent to the client after rewriting) every
// call — cheap (one small file, always overwritten, never accumulates) and
// means whatever hls.js actually choked on for a given viewer_session_id is
// sitting on disk the moment its next levelParsingError gets reported,
// regardless of whether the content passed any particular sanity check.
// Added after the #EXTM3U-prefix guard below failed to catch a real
// levelParsingError — the corruption (if any) is evidently not "file is
// empty/garbage from the first byte," so this doesn't try to guess what
// "bad" looks like at all, it just always keeps the evidence.
static void dumpLastServedPlaylist(const std::string& viewer_session_id, const std::string& raw_content,
								   const std::string& rewritten_content)
{
	std::error_code dir_ec;
	std::filesystem::create_directories("./data", dir_ec);
	std::ofstream raw_dump("./data/last_playlist_" + viewer_session_id + ".raw.m3u8", std::ios::binary);
	if (raw_dump) raw_dump << raw_content;
	std::ofstream rewritten_dump("./data/last_playlist_" + viewer_session_id + ".rewritten.m3u8", std::ios::binary);
	if (rewritten_dump) rewritten_dump << rewritten_content;
}

static void serveRewrittenChannelViewerPlaylist(const std::string& viewer_session_id, const std::string& path,
												const std::string& channel_id, const std::string& bucket,
												int64_t instance_id, httplib::Response& res)
{
	std::string content;
	if (!readFileBytes(path, content))
	{
		// This 404 lands in hls.js as a response body it tries to parse as
		// M3U8 — i.e. a fatal levelParsingError, indistinguishable client-side
		// from a genuinely malformed 200. Logged (not just returned) since
		// this path previously left zero trace anywhere, including the
		// ./data dumps below, making a real occurrence unfalsifiable from the
		// client-side error alone.
		std::cerr << "[router] serveRewrittenChannelViewerPlaylist: " << path
			<< " missing/unreadable for viewer_session_id=" << viewer_session_id
			<< " channel=" << channel_id << " bucket=" << bucket << "\n";
		res.status = 404;
		res.set_content(json{{"error", "not found"}}.dump(), "application/json");
		return;
	}
	// Guard against ever handing hls.js something it can't parse as a 200 —
	// readFileBytes() succeeding only means the file existed and was
	// readable, not that it's a complete, well-formed playlist (e.g. a
	// caught-mid-write empty/partial read, or any other case that isn't
	// literal "file missing"). A malformed 200 here is what actually
	// produces hls.js's fatal levelParsingError → retry storm → stall,
	// versus this: a client that just retries its next scheduled poll.
	if (content.compare(0, 7, "#EXTM3U") != 0)
	{
		// Dump whatever we actually read — this guard exists specifically
		// because we don't yet know what a bad read looks like (empty?
		// truncated mid-line? something else?). Saved to a file (not just
		// logged) since hephaestus.log rotates/gets noisy fast and this is
		// an intermittent, hard-to-catch-live event — a dedicated file
		// under ./data survives that and is easy to grab after the fact.
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		std::error_code dir_ec;
		std::filesystem::create_directories("./data", dir_ec);
		std::string dump_path =
			"./data/bad_playlist_" + std::to_string(ms) + "_" + channel_id + "_" + bucket + ".m3u8";
		std::ofstream dump(dump_path, std::ios::binary);
		if (dump) dump << content;
		std::cerr << "[router] serveRewrittenChannelViewerPlaylist: " << path
			<< " (" << content.size() << " bytes) failed #EXTM3U check, saved to " << dump_path << "\n";
		res.status = 503;
		res.set_content(json{{"error", "not ready"}}.dump(), "application/json");
		return;
	}
	// Same no-cache contract as serveHlsFile's manifest branch — this
	// playlist is rewritten every tick for the life of the session.
	res.set_header("Cache-Control", "no-store, no-cache, must-revalidate");
	res.set_header("Pragma", "no-cache");
	std::string rewritten = rewriteChannelViewerPlaylist(content, channel_id, bucket, instance_id);
	dumpLastServedPlaylist(viewer_session_id, content, rewritten);
	res.set_content(rewritten, "application/vnd.apple.mpegurl");
}

// Video and audio are now independent streams (VodSession/VodEncodeStream —
// see their class comments) with their own segment files and their own
// static playlist, both living in the same session directory. isAudio picks
// which one of the pair this request is actually asking for.
static void serveVodPlaylist(const std::shared_ptr<VodSession>& session, bool isAudio, httplib::Response& res,
							 SegmentCache& cache)
{
	session->touch();
	auto path = session->dir() + (isAudio ? "/audio-playlist.m3u8" : "/playlist.m3u8");
	if (!std::filesystem::exists(path))
	{
		res.status = 404;
		res.set_content(json{{"error", "not ready"}}.dump(), "application/json");
		return;
	}
	serveHlsFile(path, "application/vnd.apple.mpegurl", res, cache);
}

// seg_suffix is the already zero-padded "NNNNN" component from the URL.
static void serveVodSegment(const std::shared_ptr<VodSession>& session, bool isAudio, int index,
							const std::string& seg_suffix, httplib::Response& res, SegmentCache& cache)
{
	session->touch();
	// Segments live under the (possibly shared) stream's own directory, not
	// this session's — see VodSession::videoSegmentFilePath/
	// audioSegmentFilePath's own comment. seg_suffix is unused for path
	// purposes now (kept as a param since the URL still carries it and
	// Router's logging/callers reference it), the numeric index is what
	// actually resolves the real on-disk path.
	(void)seg_suffix;
	auto path = isAudio ? session->audioSegmentFilePath(index) : session->videoSegmentFilePath(index);
	auto prep = isAudio ? session->prepareAudioSegment(index) : session->prepareSegment(index);
	std::cerr << "[router] serveVodSegment session=" << session->sessionId() << " stream=" << (isAudio ? "audio" : "video")
		<< " seg=" << index
		<< " prep=" << (prep == VodSession::SegmentPrep::Ready
							? "Ready"
							: prep == VodSession::SegmentPrep::WaitShort
							? "WaitShort"
							: prep == VodSession::SegmentPrep::WaitColdStart
							? "WaitColdStart"
							: "Failed") << "\n";
	switch (prep)
	{
		case VodSession::SegmentPrep::Ready: break; // already on disk — serve immediately, no wait
		case VodSession::SegmentPrep::WaitShort:
			// Resuming an already-warm, already-initialized head — should be fast.
			if (!waitForFile(path, 10000))
			{
				std::cerr << "[router] serveVodSegment session=" << session->sessionId() << " seg=" << index
					<< " WaitShort TIMED OUT after 10s (503)\n";
				res.status = 503;
				return;
			}
			break;
		case VodSession::SegmentPrep::WaitColdStart:
			// A fresh head just spawned (seek beyond any live head's reach) —
			// same NVENC/CUDA cold-start budget as before.
			if (!waitForFile(path, 25000))
			{
				std::cerr << "[router] serveVodSegment session=" << session->sessionId() << " seg=" << index
					<< " WaitColdStart TIMED OUT after 25s (503)\n";
				res.status = 503;
				return;
			}
			break;
		case VodSession::SegmentPrep::Failed: std::cerr << "[router] serveVodSegment session=" << session->sessionId() << " seg=" << index << " Failed (404)\n";
			res.status = 404;
			return;
	}
	serveHlsFile(path, "video/mp2t", res, cache);
}

// Extracts "http://host:port" from a request, falling back to the Host header.
static std::string baseUrl(const httplib::Request& req)
{
	auto host = req.get_header_value("Host");
	if (host.empty()) host = "localhost:8082";
	return "http://" + host;
}

// Shared stream handler: looks up / creates a session and fans the client in.
static void handleStream(const std::string& channel_id,
						 SessionManager& sessions,
						 httplib::Response& res)
{
	auto session = sessions.getOrCreate(channel_id);
	if (!session)
	{
		res.status = 503;
		res.set_content(
			json{{"error", "channel unavailable"}, {"channel_id", channel_id}}.dump(),
			"application/json");
		return;
	}

	auto sink = std::make_shared<ClientSink>();
	session->addClient(sink);

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
				// true, not false — cpp-httplib's write_content_chunked loop
				// treats a false return as Error::Canceled (a real failure),
				// not "the provider finished." done() already wrote the
				// terminating chunk; returning true here lets the loop exit
				// through its own data_available check instead, which is what
				// actually reports success. Getting this backwards doesn't
				// corrupt what's sent to the client (done() writes the
				// correct terminator either way) but it means every clean
				// stream end gets misreported as canceled in httplib's own
				// completion callback/error tracking.
				return true;
			}

			auto chunk = std::move(sink->queue.front());
			sink->queue.pop_front();
			lock.unlock();

			return data_sink.write(
				reinterpret_cast<const char*>(chunk.data()),
				chunk.size());
		},
		[sink, session](bool)
		{
			session->removeClient(sink);
		}
	);
}

void registerRoutes(httplib::Server& svr, SessionManager& sessions, VodSessionManager& vodSessions,
					PreviewSessionManager& previewSessions, ChannelViewerRegistry& channelViewers,
					KairosClient& kairos, LogBuffer& logs, const Config& cfg,
					ClientCapabilityCache& capabilityCache, SegmentCache& segmentCache)
{
	// ── Health ────────────────────────────────────────────────────────────────
	svr.Get("/health", [](const httplib::Request&, httplib::Response& res)
	{
		res.set_content(
			json{{"status", "ok"}, {"service", "hephaestus"}}.dump(),
			"application/json");
	});

	// ── Log stream (SSE — same contract as Kairos /api/logs/stream) ───────────
	// Internal-only: the sole caller is Hermes's relayUpstreamLogs (see
	// hermes/src/main.cpp), a server-to-server merge into its own unified
	// /api/logs/stream — no browser client ever hits this directly. Same
	// shared-secret check Kairos uses for its own internal-only routes
	// (POST .../played, PUT .../keyframes); unlike Kairos's own log stream
	// there's no admin-session fallback needed here since nothing else calls it.
	svr.Get("/api/logs/stream", [&logs, &cfg](const httplib::Request& req, httplib::Response& res)
	{
		const std::string expected = readKairosInternalToken(cfg.kairos_conf_path);
		if (expected.empty() || req.get_header_value("X-Internal-Token") != expected)
		{
			res.status = 401;
			res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
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

	// Raw on-disk log file — every line since the last rotation (LogBuffer's
	// kMaxFileSize, 10MB), unlike /api/logs/stream's in-memory kMax=2000-line
	// ring buffer. Backs Hermes's /api/logs/export "download all logs"
	// diagnostics zip, which aggregates this same route from all three
	// services — same internal-only auth as /api/logs/stream just above.
	svr.Get("/api/logs/file", [&logs, &cfg](const httplib::Request& req, httplib::Response& res)
	{
		const std::string expected = readKairosInternalToken(cfg.kairos_conf_path);
		if (expected.empty() || req.get_header_value("X-Internal-Token") != expected)
		{
			res.status = 401;
			res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
			return;
		}
		std::string path = logs.path();
		if (path.empty() || !std::filesystem::exists(path))
		{
			res.status = 404;
			res.set_content(json{{"error", "no log file"}}.dump(), "application/json");
			return;
		}
		std::ifstream f(path, std::ios::binary);
		std::ostringstream ss;
		ss << f.rdbuf();
		res.set_content(ss.str(), "text/plain");
	});

	// ── HDHomeRun device emulation ────────────────────────────────────────────
	// Plex, Emby, and Jellyfin all speak the HDHomeRun HTTP API for live TV
	// discovery. Add the device manually in the DVR settings by pointing at
	// http://<hephaestus-host>:<port>. No SSDP required for manual entry.

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

	// lineup.json: one entry per Kairos channel, stream URL points back here.
	// Uses the .ts suffix so Plex doesn't try to negotiate HLS.
	svr.Get("/lineup.json", [&kairos, &sessions](
			const httplib::Request& req, httplib::Response& res)
			{
				auto base     = baseUrl(req);
				auto channels = kairos.getChannels();

				json lineup = json::array();
				for (auto& ch : channels)
				{
					lineup.push_back({
						{"GuideNumber", std::to_string(ch.number)},
						{"GuideName", ch.name},
						{"URL", base + "/stream/channels/" + ch.channel_id + ".ts"},
					});
				}

				if (lineup.empty())
				{
					lineup.push_back({
						{"GuideNumber", "1"},
						{"GuideName", "No channels — check Kairos"},
						{"URL", base + "/health"},
					});
				}

				res.set_content(lineup.dump(), "application/json");
			});

	// ── MPEG-TS stream ────────────────────────────────────────────────────────
	// Plain UUID form — used by M3U players and direct clients.
	svr.Get(R"(/stream/channels/([^/.]+)$)", [&sessions](
			const httplib::Request& req, httplib::Response& res)
			{
				handleStream(req.matches[1], sessions, res);
			});

	// .ts suffix form — used by HDHomeRun lineup.json and Plex.
	svr.Get(R"(/stream/channels/([^/]+)\.ts$)", [&sessions](
			const httplib::Request& req, httplib::Response& res)
			{
				handleStream(req.matches[1], sessions, res);
			});

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

	// ── Internal channel list (proxied from Kairos) ───────────────────────────
	svr.Get("/api/channels", [&kairos](const httplib::Request&, httplib::Response& res)
	{
		auto channels = kairos.getChannels();
		json arr      = json::array();
		for (auto& ch : channels)
			arr.push_back({
				{"channel_id", ch.channel_id},
				{"name", ch.name},
				{"number", ch.number}
			});
		res.set_content(arr.dump(), "application/json");
	});

	// ── Live channel HLS (web player) ─────────────────────────────────────────
	// Shares the channel's single running ffmpeg encode via the tee muxer
	// (ChannelSession::hlsDir()) — same session as /stream/channels/:id, just
	// a second output. Track selection is the channel's admin-configured
	// audio_lang/subtitle_lang, not per-viewer (see plan: live is broadcast).
	svr.Get(R"(/stream/hls/channels/([^/]+)/playlist\.m3u8$)", [&sessions, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				auto session = sessions.getOrCreate(req.matches[1]);
				if (!session)
				{
					res.status = 503;
					res.set_content(json{{"error", "channel unavailable"}}.dump(), "application/json");
					return;
				}
				session->touchHls();
				auto path = session->hlsDir() + "/playlist.m3u8";
				if (!waitForFile(path))
				{
					res.status = 503;
					res.set_content(json{{"error", "not ready"}}.dump(), "application/json");
					return;
				}
				serveHlsFile(path, "application/vnd.apple.mpegurl", res, segmentCache);
			});

	svr.Get(R"(/stream/hls/channels/([^/]+)/(seg-[0-9]+\.ts)$)", [&sessions, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				auto session = sessions.getOrCreate(req.matches[1]);
				if (!session)
				{
					res.status = 404;
					return;
				}
				session->touchHls();
				std::string channel_id = req.matches[1];
				serveHlsFile(session->hlsDir() + "/" + req.matches[2].str(), "video/mp2t", res, segmentCache,
							 channel_id + "|" + ChannelSession::kDefaultBucket, kLiveCacheMaxSegmentsDefault);
			});

	// Content-addressed segment route: same shape as the legacy route above,
	// just with an explicit bucket instead of always resolving to the
	// default one. Exists so the channel-viewer playlist rewrite
	// (serveRewrittenChannelViewerPlaylist) can hand every viewer of a given
	// channel+bucket the *same* segment URL regardless of their own
	// viewer_session_id — see that function's comment for why this is what
	// actually makes Hermes-side cross-viewer caching/coalescing possible.
	// Unauthenticated like every other segment route: this is broadcast
	// schedule content, not per-viewer private data.
	// Trailing [0-9]+/ is the session's instanceId() — consumed, never
	// inspected (see rewriteChannelViewerPlaylist's comment for why it's
	// there at all). Always serves whatever session is currently live for
	// this (channel_id, bucket), regardless of which instance_id the
	// requesting URL happens to carry.
	svr.Get(R"(/stream/hls/channels/([^/]+)/([^/]+)/[0-9]+/(seg-[0-9]+\.ts)$)", [&sessions, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				std::string channel_id = req.matches[1];
				std::string bucket     = req.matches[2];
				auto session           = sessions.getOrCreate(channel_id, bucket);
				if (!session)
				{
					res.status = 404;
					return;
				}
				session->touchHls();
				size_t group_cap = bucket == ChannelSession::kNativeBucket
									   ? kLiveCacheMaxSegmentsNative
									   : kLiveCacheMaxSegmentsDefault;
				serveHlsFile(session->hlsDir() + "/" + req.matches[3].str(), "video/mp2t", res, segmentCache,
							 channel_id + "|" + bucket, group_cap);
			});

	// ── Capability-bucketed live channel HLS (per-viewer, opt-in) ─────────────
	// Additive: the legacy /stream/hls/channels/:id/... routes above are
	// completely unchanged and remain what MPEG-TS/HDHomeRun/any
	// not-yet-updated client uses. This is the opt-in path a client calls to
	// get bucketed onto a copy-only ("native") session instead of the
	// universal transcode when its declared capabilities allow it — see
	// ChannelViewerRegistry's class comment.
	svr.Post(R"(/stream/channel/([^/]+)/start$)", [&sessions, &channelViewers, &capabilityCache](
			 const httplib::Request& req, httplib::Response& res)
			 {
				 std::string channel_id = req.matches[1];

				 auto token = extractBearerToken(req);
				 ClientCapabilities caps;
				 if (!token.empty()) { if (auto c = capabilityCache.get(token)) caps = *c; }

				 // Reuse whichever bucket is already running to read back the
				 // channel's authoritative resolved audio track (instead of
				 // guessing track 0) — reaching for the default bucket
				 // unconditionally here used to force a redundant transcode
				 // session into existence for every viewer, even ones who go on
				 // to resolve to the native bucket below, so the channel showed
				 // both buckets "always" active regardless of who was actually
				 // watching. Only a genuinely cold channel (neither bucket
				 // running for anyone yet) needs a fresh spawn to learn this.
				 auto session = sessions.peek(channel_id, ChannelSession::kDefaultBucket);
				 if (!session) session = sessions.peek(channel_id, ChannelSession::kNativeBucket);
				 if (!session) session = sessions.getOrCreate(channel_id);
				 if (!session)
				 {
					 res.status = 503;
					 res.set_content(json{{"error", "channel unavailable"}}.dump(), "application/json");
					 return;
				 }

				 auto info       = session->currentMediaInfo();
				 int audio_track = session->currentAudioTrack();

				 auto result = channelViewers.start(channel_id, caps, info ? &*info : nullptr, audio_track);

				 res.set_content(json{
									 {"viewer_session_id", result.viewer_session_id},
									 {"manifest_url", "/stream/hls/channel-viewer/" + result.viewer_session_id + "/playlist.m3u8"},
									 {"direct_stream", result.bucket == ChannelSession::kNativeBucket},
								 }.dump(), "application/json");
			 });

	svr.Get(R"(/stream/hls/channel-viewer/([^/]+)/playlist\.m3u8$)", [&sessions, &channelViewers](
			const httplib::Request& req, httplib::Response& res)
			{
				std::string channel_id;
				auto bucket = channelViewers.touch(req.matches[1], channel_id);
				if (bucket.empty())
				{
					// hls.js treats any non-M3U8 response body (including this
					// JSON error) as a fatal levelParsingError — logged so a
					// live occurrence is distinguishable from the other
					// non-200 branches below instead of leaving the client
					// error as the only evidence.
					std::cerr << "[router] channel-viewer playlist: viewer session not found for "
						<< req.matches[1].str() << "\n";
					res.status = 404;
					res.set_content(json{{"error", "viewer session not found"}}.dump(), "application/json");
					return;
				}
				auto session = sessions.getOrCreate(channel_id, bucket);
				if (!session)
				{
					std::cerr << "[router] channel-viewer playlist: channel unavailable for viewer_session_id="
						<< req.matches[1].str() << " channel=" << channel_id << " bucket=" << bucket << "\n";
					res.status = 503;
					res.set_content(json{{"error", "channel unavailable"}}.dump(), "application/json");
					return;
				}
				session->touchHls();
				auto path = session->hlsDir() + "/playlist.m3u8";
				if (!waitForFile(path))
				{
					std::cerr << "[router] channel-viewer playlist: " << path
						<< " never appeared for viewer_session_id=" << req.matches[1].str()
						<< " channel=" << channel_id << " bucket=" << bucket << "\n";
					res.status = 503;
					res.set_content(json{{"error", "not ready"}}.dump(), "application/json");
					return;
				}
				std::string viewer_session_id = req.matches[1];
				serveRewrittenChannelViewerPlaylist(viewer_session_id, path, channel_id, bucket, session->instanceId(), res);
			});

	svr.Get(R"(/stream/hls/channel-viewer/([^/]+)/(seg-[0-9]+\.ts)$)", [&sessions, &channelViewers, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				std::string channel_id;
				auto bucket = channelViewers.touch(req.matches[1], channel_id);
				if (bucket.empty())
				{
					res.status = 404;
					return;
				}
				auto session = sessions.getOrCreate(channel_id, bucket);
				if (!session)
				{
					res.status = 404;
					return;
				}
				session->touchHls();
				size_t group_cap = bucket == ChannelSession::kNativeBucket
									   ? kLiveCacheMaxSegmentsNative
									   : kLiveCacheMaxSegmentsDefault;
				serveHlsFile(session->hlsDir() + "/" + req.matches[2].str(), "video/mp2t", res, segmentCache,
							 channel_id + "|" + bucket, group_cap);
			});

	svr.Post(R"(/stream/channel/viewer/([^/]+)/stop$)", [&channelViewers](
			 const httplib::Request& req, httplib::Response& res)
			 {
				 channelViewers.stop(req.matches[1]);
				 res.set_content(json{{"ok", true}}.dump(), "application/json");
			 });

	// Polled by Hades (usePlaybackSession.ts) to decide whether to reconnect
	// this viewer onto a different bucket — see ChannelViewerRegistry's own
	// comment for why a live bucket flip must go through a real reconnect
	// (fresh viewer_session_id + manifest_url) instead of silently swapping
	// which session an existing manifest URL resolves to.
	svr.Get(R"(/stream/channel/viewer/([^/]+)/status$)", [&channelViewers](
			const httplib::Request& req, httplib::Response& res)
			{
				auto st = channelViewers.status(req.matches[1]);
				if (!st)
				{
					res.status = 404;
					res.set_content(json{{"error", "viewer session not found"}}.dump(), "application/json");
					return;
				}
				res.set_content(json{
									{"bucket", st->bucket},
									{"recommended_bucket", st->recommended_bucket},
									{"reconnect_recommended", st->recommended_bucket != st->bucket},
								}.dump(), "application/json");
			});

	// ── VOD (on-demand library playback) ──────────────────────────────────────
	// One session per viewer, one session_id/manifest for its whole life. A
	// TRACK SWITCH is still "stop this session, start a fresh one" (a
	// different audio/subtitle selection needs genuinely different ffmpeg
	// -map args). A plain SEEK is not — VodSession restarts its own internal
	// encoder in place (see VodSession::restartAt/prepareSegment) and keeps
	// serving the same manifest the whole time.
	svr.Post("/stream/vod/start", [&kairos, &vodSessions, &capabilityCache](
			 const httplib::Request& req, httplib::Response& res)
			 {
				 json body;
				 try { body = json::parse(req.body); }
				 catch (...)
				 {
					 res.status = 400;
					 res.set_content(json{{"error", "invalid JSON"}}.dump(), "application/json");
					 return;
				 }
				 auto content_type = body.value("content_type", "");
				 auto content_id   = body.value("content_id", "");
				 if (content_type != "movie" && content_type != "episode")
				 {
					 res.status = 400;
					 res.set_content(json{{"error", "content_type must be movie or episode"}}.dump(), "application/json");
					 return;
				 }

				 auto token = extractBearerToken(req);
				 // Multi-part movies (GitHub #3): Hades passes part_num once it's
				 // resolved which part to start in (GET /api/movies/:id/resolve-
				 // play-target) — forwarded straight through to Kairos's ?part=.
				 int part_num = body.value("part_num", 0);
				 auto info    = kairos.getPlaybackInfo(content_type, content_id, token, part_num);
				 if (!info || info->file_path.empty())
				 {
					 res.status = 404;
					 res.set_content(json{{"error", "content not found"}}.dump(), "application/json");
					 return;
				 }

				 int audio_track     = body.value("audio_track", -1);
				 int subtitle_track  = body.value("subtitle_track", -1);
				 int64_t position_ms = body.value("position_ms", int64_t(0));
				 if (position_ms < 0) position_ms = 0;
				 bool hdr_capable = body.value("hdr_capable", false);

				 std::optional<ClientCapabilities> client_caps;
				 if (!token.empty()) client_caps = capabilityCache.get(token);

				 auto session = vodSessions.create(content_type, content_id, info->file_path, position_ms, audio_track,
												   subtitle_track, hdr_capable, client_caps, info->external_subtitles,
												   info->duration_ms, info->preferred_audio_lang, info->preferred_subtitle_lang,
												   info->keyframes_ms, info->keyframes_size, info->keyframes_mtime);
				 if (!session)
				 {
					 // Covers both a genuine probe/start failure and the concurrent-
					 // session cap (see VodSessionManager::create) — 503 either way
					 // since both are "try again," not a client-input error.
					 res.status = 503;
					 res.set_content(json{{"error", "failed to start playback (or server is at capacity — try again shortly)"}}.dump(), "application/json");
					 return;
				 }

				 json tracks;
				 tracks["video"] = json::array();
				 for (auto& t : session->tracks().video) tracks["video"].push_back({{"codec", t.codec}, {"width", t.width}, {"height", t.height}});
				 tracks["audio"] = json::array();
				 for (auto& t : session->tracks().audio) tracks["audio"].push_back({{"index", t.relative_index}, {"codec", t.codec}, {"language", t.language}, {"title", t.title}, {"channels", t.channels}});
				 tracks["subtitles"] = json::array();
				 for (auto& t : session->tracks().subtitles)
				 {
					 bool extractable = t.codec == "subrip" || t.codec == "ass" || t.codec == "ssa" ||
						 t.codec == "mov_text" || t.codec == "webvtt" || t.codec == "text";
					 // Bitmap formats (Blu-ray/DVD/DVB) aren't extractable as text,
					 // but can be burned directly into the video instead — see
					 // VodSession.cpp's isBitmapSubtitleCodec/subtitleBurnIn.
					 bool burnIn = t.codec == "hdmv_pgs_subtitle" || t.codec == "dvd_subtitle" || t.codec == "dvb_subtitle";
					 tracks["subtitles"].push_back({
						 {"index", t.relative_index}, {"codec", t.codec}, {"language", t.language}, {"title", t.title},
						 {"extractable", extractable}, {"burn_in", burnIn}, {"source", "embedded"}
					 });
				 }
				 // External sidecar files get negative indices starting at -2 (-1 is
				 // already "no subtitle" on the wire — see usePlaybackSession.ts/
				 // TrackMenu.tsx) — assigned here, once, in catalog order; VodSession::
				 // start()'s subtitle_track<=-2 branch reverses this exact formula to
				 // resolve a selection back to the right file. Nothing but this
				 // shared convention keeps the two in sync, so don't reorder either
				 // side independently.
				 int ext_index = -2;
				 for (auto& t : session->externalSubtitles())
				 {
					 tracks["subtitles"].push_back({
						 {"index", ext_index--}, {"codec", ""}, {"language", t.language}, {"title", t.title},
						 {"extractable", true}, {"burn_in", false}, {"source", "external"},
						 {"forced", t.forced}, {"sdh", t.sdh}
					 });
				 }

				 json out = {
					 {"session_id", session->sessionId()},
					 // The multi-rendition master manifest (AUDIO/SUBTITLES groups —
					 // see VodSession::buildMasterPlaylist), not the flat variant
					 // playlist directly. hls.js's loadSource() already handles both
					 // single- and multi-variant playlists transparently. Native
					 // players (Android/TV) get the real groups to build their own
					 // language picker from; Hades drives its own TrackMenu against
					 // hls.js's subtitleTrack/audioTrack APIs instead (see
					 // VideoPlayer.tsx), matched against these renditions by URL.
					 {"manifest_url", "/stream/vod/" + session->sessionId() + "/master.m3u8"},
					 {"direct_stream", session->directStream()},
					 // Prefer this session's own authoritative ffprobe duration —
					 // Kairos's info->duration_ms is only the fallback VodSession uses
					 // when its own probe comes back empty (see durationMs()'s comment).
					 {"duration_ms", session->durationMs() > 0 ? session->durationMs() : info->duration_ms},
					 {"title", info->title},
					 {"tracks", tracks},
					 {"subtitle_burned_in", session->subtitleBurnedIn()},
					 // The actually-resolved selection (VodSession::audioTrack()/
					 // subtitleTrack()'s own comment) — may differ from what the
					 // request asked for (e.g. -1/"unset" resolved to a saved
					 // preference or the first track), and nothing else in this
					 // response says which master-manifest rendition is already
					 // active, so callers driving the manifest directly need this to
					 // start their own selection state in sync with it.
					 {"audio_track", session->audioTrack()},
					 {"subtitle_track", session->subtitleTrack()},
					 {"is_multi_part", info->is_multi_part},
				 };
				 if (info->is_multi_part)
				 {
					 out["part_num"]          = info->part_num;
					 out["total_parts"]       = info->total_parts;
					 out["movie_duration_ms"] = info->movie_duration_ms;
				 }
				 if (session->hasSubtitleOutput()) out["subtitle_url"] = "/stream/vod/" + session->sessionId() + "/subs.vtt";
				 res.set_content(out.dump(), "application/json");
			 });

	svr.Get(R"(/stream/vod/([^/]+)/master\.m3u8$)", [&vodSessions](
			const httplib::Request& req, httplib::Response& res)
			{
				auto session = vodSessions.get(req.matches[1]);
				if (!session)
				{
					res.status = 404;
					res.set_content(json{{"error", "session not found"}}.dump(), "application/json");
					return;
				}
				session->touch();
				res.set_content(session->buildMasterPlaylist(), "application/vnd.apple.mpegurl");
			});

	svr.Get(R"(/stream/vod/([^/]+)/playlist\.m3u8$)", [&vodSessions, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				auto session = vodSessions.get(req.matches[1]);
				if (!session)
				{
					res.status = 404;
					res.set_content(json{{"error", "session not found"}}.dump(), "application/json");
					return;
				}
				// No wait needed anymore: VodSession writes the complete, static
				// playlist.m3u8 synchronously inside start(), which itself runs
				// synchronously inside POST /stream/vod/start — before that response
				// (and so the manifest_url it contains) can possibly reach the
				// client. The old 25s NVENC-cold-start-aware wait here is gone; only
				// a defensive existence check remains for the (should-be-rare) case
				// the session already vanished by the time this GET lands.
				serveVodPlaylist(session, /*isAudio=*/false, res, segmentCache);
			});

	svr.Get(R"(/stream/vod/([^/]+)/seg-([0-9]+)\.ts$)", [&vodSessions, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				auto session = vodSessions.get(req.matches[1]);
				if (!session)
				{
					res.status = 404;
					return;
				}
				int index = std::stoi(req.matches[2].str());
				serveVodSegment(session, /*isAudio=*/false, index, req.matches[2].str(), res, segmentCache);
			});

	// Per-audio-track alias for the master manifest's AUDIO group — see
	// VodSession::ensureAudioTrack's comment. A native player picking a
	// non-active language hits this before its own segments, which is what
	// actually triggers the encoder restart onto the new -map; the segment
	// route below is a defensive fallback for a cached URL fetched without
	// re-fetching the playlist first.
	svr.Get(R"(/stream/vod/([^/]+)/audio/([0-9]+)/playlist\.m3u8$)", [&vodSessions, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				std::string sid = req.matches[1];
				int track       = std::stoi(req.matches[2].str());
				std::cerr << "[router] GET audio playlist.m3u8 session=" << sid << " track=" << track
					<< " UA=\"" << req.get_header_value("User-Agent") << "\"\n";
				auto session = vodSessions.get(sid);
				if (!session)
				{
					res.status = 404;
					res.set_content(json{{"error", "session not found"}}.dump(), "application/json");
					return;
				}
				if (!session->ensureAudioTrack(track))
				{
					std::cerr << "[router] audio playlist.m3u8 session=" << sid << " track=" << track << " ensureAudioTrack FAILED\n";
					res.status = 500;
					res.set_content(json{{"error", "failed to switch audio track"}}.dump(), "application/json");
					return;
				}
				serveVodPlaylist(session, /*isAudio=*/true, res, segmentCache);
			});

	svr.Get(R"(/stream/vod/([^/]+)/audio/([0-9]+)/aseg-([0-9]+)\.ts$)", [&vodSessions, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				std::string sid = req.matches[1];
				int track       = std::stoi(req.matches[2].str());
				std::cerr << "[router] GET audio seg session=" << sid << " track=" << track
					<< " seg=" << req.matches[3].str() << " UA=\"" << req.get_header_value("User-Agent") << "\"\n";
				auto session = vodSessions.get(sid);
				if (!session)
				{
					res.status = 404;
					return;
				}
				if (!session->ensureAudioTrack(track))
				{
					std::cerr << "[router] audio seg session=" << sid << " track=" << track << " ensureAudioTrack FAILED\n";
					res.status = 500;
					return;
				}
				int index = std::stoi(req.matches[3].str());
				serveVodSegment(session, /*isAudio=*/true, index, req.matches[3].str(), res, segmentCache);
			});

	// Wraps the on-demand pipe below in a minimal single-segment VOD media
	// playlist — an #EXT-X-MEDIA URI must reference a Media Playlist per the
	// HLS spec, not a raw resource. Pointing the master manifest's
	// SUBTITLES group straight at the pipe route made hls.js fail to parse
	// it as a playlist and retry in a tight, un-backed-off loop (observed
	// spawning the same extraction repeatedly within a fraction of a
	// second). This route is fetched once; the one #EXTINF segment it
	// declares is fetched once by the route below — same "fetched exactly
	// once" shape /subs.vtt always had.
	svr.Get(R"(/stream/vod/([^/]+)/subtitles/(-?[0-9]+)/playlist\.m3u8$)", [&vodSessions](
			const httplib::Request& req, httplib::Response& res)
			{
				std::string sid = req.matches[1];
				int track       = std::stoi(req.matches[2].str());
				std::cerr << "[router] GET subtitles playlist.m3u8 session=" << sid << " track=" << track << "\n";
				auto session = vodSessions.get(sid);
				if (!session)
				{
					std::cerr << "[router] subtitles playlist.m3u8 session=" << sid << " not found\n";
					res.status = 404;
					res.set_content(json{{"error", "session not found"}}.dump(), "application/json");
					return;
				}
				session->touch();
				auto playlist = session->buildSubtitlePlaylist(track);
				if (!playlist)
				{
					std::cerr << "[router] subtitles playlist.m3u8 session=" << sid << " track=" << track << " unavailable (404)\n";
					res.status = 404;
					res.set_content(json{{"error", "subtitle track not available"}}.dump(), "application/json");
					return;
				}
				res.set_content(*playlist, "application/vnd.apple.mpegurl");
			});

	// On-demand WebVTT pipe for any subtitle track (embedded relative_index
	// >= 0, external sidecar -2/-3/... — same convention as the `tracks`
	// JSON below), referenced as the one segment in the playlist route
	// above. Streams ffmpeg's stdout straight to the client instead of
	// extracting to disk first (buildVodSubtitleArgs' on_fly path) — no
	// up-front wait for the whole extraction to finish the way /subs.vtt
	// needs. Fully independent of audio track switching: never touches the
	// main encoder.
	svr.Get(R"(/stream/vod/([^/]+)/subtitles/(-?[0-9]+)$)", [&vodSessions](
			const httplib::Request& req, httplib::Response& res)
			{
				std::string sid = req.matches[1];
				int track       = std::stoi(req.matches[2].str());
				std::cerr << "[router] GET subtitles pipe session=" << sid << " track=" << track << "\n";
				auto session = vodSessions.get(sid);
				if (!session)
				{
					std::cerr << "[router] subtitles pipe session=" << sid << " not found\n";
					res.status = 404;
					return;
				}
				session->touch();

				auto sink = std::make_shared<ClientSink>();

				// HLS players anchor a WebVTT segment's cue clock onto the shared
				// presentation timeline via X-TIMESTAMP-MAP — without it, a player
				// can fall back to treating cue time 0 as "whenever this segment
				// started loading" instead of the file's real absolute time, which
				// is the domain buildVodSubtitleArgs' cues actually use (no -ss —
				// always the whole file). Observed in practice as subtitles running
				// from the episode's start regardless of where the video itself
				// resumed. ffmpeg's webvtt muxer doesn't emit this header, so it's
				// spliced in here: push our own as this response's first chunk, then
				// strip ffmpeg's own literal "WEBVTT\n\n" signature (libavformat's
				// webvttenc.c writes exactly that, unconditionally) off the front of
				// its real output below so the result is one valid WEBVTT file, not
				// two concatenated ones.
				static const std::string kWebvttSignature = "WEBVTT\n\n";
				static const std::string kWebvttHeader    =
					"WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:0\n\n";
				{
					std::lock_guard<std::mutex> lock(sink->mtx);
					sink->queue.emplace_back(kWebvttHeader.begin(), kWebvttHeader.end());
				}
				auto skip_remaining = std::make_shared<size_t>(kWebvttSignature.size());
				// Byte-count diagnostic — an ffmpeg run that exits 0 but produced
				// near-zero actual cue bytes (e.g. its srt demuxer silently parsing
				// zero blocks out of a mis-detected charenc, rather than erroring)
				// would otherwise look identical in the logs to a real successful
				// extraction; this makes that distinction visible.
				auto bytes_out = std::make_shared<std::atomic<size_t>>(0);

				std::shared_ptr<FfmpegProcess> proc;
				try
				{
					// spawnSubtitlePipe forks + starts two reader threads — under
					// resource pressure (e.g. a client hammering this route) thread
					// creation can throw std::system_error; letting that escape an
					// httplib handler is fatal to the whole process, not just this
					// request.
					proc = session->spawnSubtitlePipe(track,
													  [sink, skip_remaining, bytes_out](const uint8_t* data, size_t len)
													  {
														  if (*skip_remaining > 0)
														  {
															  size_t skip     = std::min(*skip_remaining, len);
															  data            += skip;
															  len             -= skip;
															  *skip_remaining -= skip;
														  }
														  if (len == 0) return;
														  bytes_out->fetch_add(len);
														  std::lock_guard<std::mutex> lock(sink->mtx);
														  if (sink->queue.size() < ClientSink::MAX_QUEUE) sink->queue.emplace_back(data, data + len);
														  sink->cv.notify_one();
													  },
													  [sink, bytes_out, sid, track](int code)
													  {
														  std::cerr << "[router] subtitles pipe session=" << sid << " track=" << track
															  << " ffmpeg exited code=" << code << " total cue bytes=" << bytes_out->load() << "\n";
														  std::lock_guard<std::mutex> lock(sink->mtx);
														  sink->done.store(true);
														  sink->cv.notify_one();
													  });
				}
				catch (const std::exception& e)
				{
					std::cerr << "[router] subtitles pipe session=" << sid << " track=" << track
						<< " spawn threw: " << e.what() << "\n";
					res.status = 503;
					res.set_content(json{{"error", "subtitle track temporarily unavailable"}}.dump(), "application/json");
					return;
				}
				if (!proc)
				{
					std::cerr << "[router] subtitles pipe session=" << sid << " track=" << track << " spawn returned null (404)\n";
					res.status = 404;
					res.set_content(json{{"error", "subtitle track not available"}}.dump(), "application/json");
					return;
				}
				std::cerr << "[router] subtitles pipe session=" << sid << " track=" << track
					<< " streaming response (chunked, text/vtt)\n";

				res.set_chunked_content_provider(
					"text/vtt",
					[sink](size_t, httplib::DataSink& data_sink) -> bool
					{
						std::unique_lock<std::mutex> lock(sink->mtx);
						sink->cv.wait(lock, [&] { return !sink->queue.empty() || sink->done.load(); });
						// true, not false — see the /stream/channels/:id route's
						// identical done()/return pair for why: httplib's
						// write_content_chunked treats a false return as
						// Error::Canceled regardless of done() having already
						// written the real terminator, so this was misreporting
						// every clean finish as a cancellation in the on-complete
						// callback below.
						if (sink->queue.empty())
						{
							data_sink.done();
							return true;
						}
						auto chunk = std::move(sink->queue.front());
						sink->queue.pop_front();
						lock.unlock();
						return data_sink.write(reinterpret_cast<const char*>(chunk.data()), chunk.size());
					},
					[proc, sid, track](bool success)
					{
						// proc (the last reference) is destroyed here, killing the
						// ffmpeg process if the client disconnected before it
						// finished on its own — see FfmpegProcess's destructor.
						std::cerr << "[router] subtitles pipe session=" << sid << " track=" << track
							<< " response finished success=" << (success ? "yes" : "no") << "\n";
					}
				);
			});

	svr.Get(R"(/stream/vod/([^/]+)/subs\.vtt$)", [&vodSessions, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				auto session = vodSessions.get(req.matches[1]);
				if (!session)
				{
					res.status = 404;
					return;
				}
				session->touch();
				// Lazy — see VodSession::spawnSubtitleProcess()'s own comment. No
				// longer spawned unconditionally at session start, since nothing
				// built against this codebase still uses this route (both Hades and
				// Android moved to the per-track on-demand /subtitles/{n} pipe) —
				// idempotent, so harmless if something else already triggered it.
				session->spawnSubtitleProcess();
				auto path = session->dir() + "/subs.vtt";
				if (!waitForFile(path, 3000))
				{
					res.status = 503;
					return;
				} // existence check, now that a first request just triggered the spawn above
				// Completeness — see waitForSubtitleExtraction's own comment. Must
				// actually check the result: a <track>/ExoPlayer sideloaded fetch
				// happens exactly once and never re-fetches, so serving whatever
				// partial bytes ffmpeg has flushed so far (rather than 503ing and
				// letting the client's own request logic decide whether to retry)
				// means permanently truncated/no cues for the rest of the file, not
				// a merely-late-but-eventually-correct one.
				if (!waitForSubtitleExtraction(*session))
				{
					res.status = 503;
					return;
				}
				// Exited doesn't mean succeeded — ffmpeg can leave a syntactically
				// valid but empty/truncated WebVTT file behind on failure (e.g. its
				// srt demuxer rejecting non-UTF-8 input despite sniffSubCharenc's
				// mitigation). Serving that as 200 reads to the client as "this
				// title just has no subtitles," silently, instead of a real error.
				if (session->hasSubtitleExtractionFailed())
				{
					res.status = 500;
					res.set_content(json{{"error", "subtitle extraction failed"}}.dump(), "application/json");
					return;
				}
				serveHlsFile(path, "text/vtt", res, segmentCache);
			});

	svr.Post(R"(/stream/vod/([^/]+)/stop$)", [&vodSessions](
			 const httplib::Request& req, httplib::Response& res)
			 {
				 vodSessions.stop(req.matches[1]);
				 res.set_content(json{{"ok", true}}.dump(), "application/json");
			 });

	// ── Preview (Guide hover previews) ────────────────────────────────────────
	svr.Post("/stream/preview/start", [&previewSessions](
			 const httplib::Request& req, httplib::Response& res)
			 {
				 json body;
				 try { body = json::parse(req.body); }
				 catch (...)
				 {
					 res.status = 400;
					 res.set_content(json{{"error", "invalid JSON"}}.dump(), "application/json");
					 return;
				 }
				 auto channel_id = body.value("channel_id", "");
				 if (channel_id.empty())
				 {
					 res.status = 400;
					 res.set_content(json{{"error", "channel_id required"}}.dump(), "application/json");
					 return;
				 }
				 bool hdr_capable = body.value("hdr_capable", false);
				 auto session     = previewSessions.create(channel_id, hdr_capable);
				 if (!session)
				 {
					 // See /stream/vod/start's own comment — same cap-or-failure ambiguity.
					 res.status = 503;
					 res.set_content(json{{"error", "failed to start preview (or server is at capacity — try again shortly)"}}.dump(), "application/json");
					 return;
				 }
				 res.set_content(json{
									 {"session_id", session->sessionId()},
									 {"manifest_url", "/stream/preview/" + session->sessionId() + "/playlist.m3u8"},
								 }.dump(), "application/json");
			 });

	svr.Post(R"(/stream/preview/([^/]+)/switch$)", [&previewSessions](
			 const httplib::Request& req, httplib::Response& res)
			 {
				 json body;
				 try { body = json::parse(req.body); }
				 catch (...)
				 {
					 res.status = 400;
					 res.set_content(json{{"error", "invalid JSON"}}.dump(), "application/json");
					 return;
				 }
				 auto channel_id = body.value("channel_id", "");
				 if (channel_id.empty())
				 {
					 res.status = 400;
					 res.set_content(json{{"error", "channel_id required"}}.dump(), "application/json");
					 return;
				 }
				 if (!previewSessions.switchChannel(req.matches[1], channel_id))
				 {
					 res.status = 404;
					 res.set_content(json{{"error", "session not found or switch failed"}}.dump(), "application/json");
					 return;
				 }
				 res.set_content(json{{"ok", true}}.dump(), "application/json");
			 });

	svr.Get(R"(/stream/preview/([^/]+)/playlist\.m3u8$)", [&previewSessions, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				auto session = previewSessions.get(req.matches[1]);
				if (!session)
				{
					res.status = 404;
					res.set_content(json{{"error", "session not found"}}.dump(), "application/json");
					return;
				}
				session->touch();
				auto path = session->dir() + "/playlist.m3u8";
				if (!waitForFile(path))
				{
					res.status = 503;
					res.set_content(json{{"error", "not ready"}}.dump(), "application/json");
					return;
				}
				serveHlsFile(path, "application/vnd.apple.mpegurl", res, segmentCache);
			});

	svr.Get(R"(/stream/preview/([^/]+)/(seg-[0-9]+\.ts)$)", [&previewSessions, &segmentCache](
			const httplib::Request& req, httplib::Response& res)
			{
				auto session = previewSessions.get(req.matches[1]);
				if (!session)
				{
					res.status = 404;
					return;
				}
				session->touch();
				serveHlsFile(session->dir() + "/" + req.matches[2].str(), "video/mp2t", res, segmentCache);
			});

	svr.Post(R"(/stream/preview/([^/]+)/stop$)", [&previewSessions](
			 const httplib::Request& req, httplib::Response& res)
			 {
				 previewSessions.stop(req.matches[1]);
				 res.set_content(json{{"ok", true}}.dump(), "application/json");
			 });
}