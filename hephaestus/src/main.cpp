#include "Config.h"
#include "api/ActivityRouter.h"
#include "api/ClientCapabilitiesRouter.h"
#include "api/Router.h"
#include "crash/CrashHandler.h"
#include "kairos/KairosClient.h"
#include "log/LogBuffer.h"
#include "log/RuntimeFlags.h"
#include "stream/ChannelViewerRegistry.h"
#include "stream/ClientCapabilities.h"
#include "stream/EncoderArgs.h" // hwAccelName
#include "stream/HwProbe.h"
#include "stream/EncoderAdmission.h"
#include "stream/SessionManager.h"
#include "stream/VodSessionManager.h"
#include "stream/PreviewSessionManager.h"
#include "stream/CacheSizing.h"
#include "cache/SegmentCache.h"
#include <httplib.h>
#include <iostream>

int main(int argc, char* argv[])
{
	// Intercept cout/cerr before anything else so startup messages are captured.
	LogBuffer log_buffer;
	LogTee tee_cout(std::cout, log_buffer);
	LogTee tee_cerr(std::cerr, log_buffer);
	log_buffer.setFile("./data/hephaestus.log");
	log_buffer.setFilter(hephaestusLogFilter);
	installCrashHandlers("hephaestus", "./data", &log_buffer);

	Config cfg = parseConfig(argc, argv);

	// Verifies the configured GPU backend actually works before any session
	// is ever built with it, instead of every stream discovering that the
	// hard way at spawn time. Resolved once, here, and copied into every
	// *StreamOptions below — sessions never re-probe or retry.
	HwCapabilities hw_caps = probeHwCapabilities(cfg.hw_accel, cfg.ffmpeg_path,
												 cfg.vaapi_device, cfg.hw_probe_assets_dir,
												 cfg.verbose_transcode_logs);
	std::cout << "[hephaestus] hw-accel: requested=" << hwAccelName(cfg.hw_accel)
		<< " encode=" << hwAccelName(hw_caps.encode)
		<< " decode=" << hwAccelName(hw_caps.decode)
		<< " decodable_codecs=" << hw_caps.decodable_codecs.size() << "\n";

	KairosClient kairos(cfg.kairos_url, cfg.kairos_conf_path);

	// In-memory segment cache — one process-wide instance, budget from
	// cfg.segment_cache_mb (0 = disabled, the default). See
	// shared/cache/SegmentCache.h for the disabled fast-path.
	SegmentCache segment_cache(cfg.segment_cache_mb * 1024 * 1024);
	{
		auto channels       = kairos.getChannels();
		size_t suggested_mb = CacheSizing::suggestSegmentCacheMb(channels);
		std::cout << "[cache] " << channels.size() << " channels configured; suggested "
			<< "HEPH_SEGMENT_CACHE_MB >= " << suggested_mb
			<< " (segment_cache_mb=" << cfg.segment_cache_mb << "). This floor is shared across "
			<< "all viewers of each channel (one ffmpeg process per channel+bucket) — it scales "
			<< "with distinct channels actively watched, not with total viewer count. Direct-play "
			<< "(\"native\" bucket) and VOD scrubbing draw on the same budget opportunistically "
			<< "above this floor and are self-limiting (LRU eviction, no hard failure) if it's not "
			<< "raised further.\n";
	}

	// One instance shared by every session type below — see its own class
	// comment for why the per-type max_vod_sessions/max_preview_sessions
	// caps (and channels' complete lack of one) don't already cover this.
	EncoderAdmission encoder_admission(cfg.max_gpu_encode_sessions);

	StreamOptions stream_opts;
	stream_opts.ffprobe_path           = cfg.ffprobe_path;
	stream_opts.audio_lang             = cfg.audio_lang;
	stream_opts.loudnorm               = cfg.loudnorm;
	stream_opts.ffmpeg_debug_logs      = cfg.ffmpeg_debug_logs;
	stream_opts.verbose_transcode_logs = cfg.verbose_transcode_logs;
	stream_opts.linger_secs            = cfg.session_linger_secs;
	stream_opts.buffer_size            = cfg.stream_buffer_size;
	stream_opts.hw_accel               = hw_caps.encode;
	stream_opts.decode_hw_accel        = hw_caps.decode;
	stream_opts.decodable_codecs       = hw_caps.decodable_codecs;
	stream_opts.vaapi_device           = cfg.vaapi_device;
	stream_opts.default_logo_path      = cfg.default_logo_path;
	stream_opts.hls_root               = cfg.hls_root;
	stream_opts.segment_cache          = &segment_cache;

	// stream_opts.buffer_size (from --buffer-size/BUF_SIZE above) is the fallback
	// if Kairos is unreachable — SessionManager fetches the persisted Kairos
	// setting itself (and keeps it fresh in the background) so later changes
	// made in Hades don't require a Hephaestus restart.
	SessionManager sessions(kairos, cfg.ffmpeg_path, stream_opts);
	// Capability-bucketed live channel HLS (see ChannelViewerRegistry's class
	// comment) — constructed after `sessions` since it needs a reference back
	// to it, then wired both ways so ChannelSession's own item-transition
	// hook (onItemPlaying) reaches it.
	ChannelViewerRegistry channelViewers(sessions);
	sessions.setViewerRegistry(&channelViewers);

	VodStreamOptions vod_opts;
	vod_opts.ffprobe_path           = cfg.ffprobe_path;
	vod_opts.hls_root               = cfg.hls_root;
	vod_opts.linger_secs            = cfg.session_linger_secs;
	vod_opts.buffer_size            = cfg.stream_buffer_size;
	vod_opts.hw_accel               = hw_caps.encode;
	vod_opts.decode_hw_accel        = hw_caps.decode;
	vod_opts.decodable_codecs       = hw_caps.decodable_codecs;
	vod_opts.vaapi_device           = cfg.vaapi_device;
	vod_opts.ffmpeg_debug_logs      = cfg.ffmpeg_debug_logs;
	vod_opts.verbose_transcode_logs = cfg.verbose_transcode_logs;
	vod_opts.lookahead_secs         = cfg.vod_lookahead_secs;
	vod_opts.segment_cache          = &segment_cache;
	VodSessionManager vodSessions(cfg.ffmpeg_path, vod_opts, kairos, cfg.max_vod_sessions);

	PreviewStreamOptions preview_opts;
	preview_opts.ffprobe_path           = cfg.ffprobe_path;
	preview_opts.hls_root               = cfg.hls_root;
	preview_opts.default_logo_path      = cfg.default_logo_path;
	preview_opts.linger_secs            = cfg.session_linger_secs;
	preview_opts.buffer_size            = cfg.stream_buffer_size;
	preview_opts.hw_accel               = hw_caps.encode;
	preview_opts.decode_hw_accel        = hw_caps.decode;
	preview_opts.decodable_codecs       = hw_caps.decodable_codecs;
	preview_opts.vaapi_device           = cfg.vaapi_device;
	preview_opts.ffmpeg_debug_logs      = cfg.ffmpeg_debug_logs;
	preview_opts.verbose_transcode_logs = cfg.verbose_transcode_logs;
	preview_opts.segment_cache          = &segment_cache;
	PreviewSessionManager previewSessions(cfg.ffmpeg_path, preview_opts, kairos, cfg.max_preview_sessions);

	httplib::Server svr;
	svr.new_task_queue = [] { return new httplib::ThreadPool(16); };
	// See Kairos's main.cpp for why — same reasoning; every body Hephaestus
	// accepts (POST /stream/vod/start etc.) is a small JSON control payload.
	svr.set_payload_max_length(25 * 1024 * 1024);

	// Baseline hardening headers on every response — see Kairos's Router.cpp
	// for why no Content-Security-Policy is included here either.
	svr.set_post_routing_handler([](const httplib::Request&, httplib::Response& res)
	{
		res.set_header("X-Content-Type-Options", "nosniff");
		res.set_header("X-Frame-Options", "DENY");
		res.set_header("Referrer-Policy", "same-origin");
		res.set_header("Strict-Transport-Security", "max-age=15552000; includeSubDomains");
	});

	// Per-client declared decode capability for VodSession's direct-stream
	// decision — see ClientCapabilities.h. One cache for the whole
	// process's lifetime, same as every *SessionManager above.
	ClientCapabilityCache capability_cache;

	registerRoutes(svr, sessions, vodSessions, previewSessions, channelViewers, kairos, log_buffer, cfg, capability_cache, segment_cache);
	registerClientCapabilitiesRoutes(svr, capability_cache);
	// Prefer encode if resolved, else decode -- either way the physical GPU
	// in question is the one whose live utilization/memory/temp the
	// Activity page cares about (see ActivityRouter.h's own comment).
	HwAccel gpu_backend = hw_caps.encode != HwAccel::none ? hw_caps.encode : hw_caps.decode;
	registerActivityRoutes(svr, sessions, vodSessions, log_buffer, gpu_backend, cfg.vaapi_device, channelViewers);

	std::cout << "[hephaestus] listening on :" << cfg.port
		<< "  kairos=" << cfg.kairos_url
		<< "  ffmpeg=" << cfg.ffmpeg_path << "\n";

	svr.listen("0.0.0.0", cfg.port);
	return 0;
}