#pragma once
#include "stream/ChannelSession.h"
#include <string>
#include <cstdlib>
#include <algorithm>

struct Config
{
	std::string kairos_url = "http://localhost:8080";
	// Path to Kairos's own kairos.conf, read directly off the /data volume
	// both services already share — see kairos/InternalToken.h for why (the
	// shared secret it holds gates POST /api/channels/:id/played and must
	// never be fetched over HTTP, authenticated or not).
	std::string kairos_conf_path = "/data/kairos.conf";
	std::string ffmpeg_path      = "ffmpeg";
	std::string ffprobe_path     = "ffprobe";
	int port                     = 8082;
	std::string audio_lang       = "eng";
	bool loudnorm                = false;
	bool ffmpeg_debug_logs       = false; // pipe ffmpeg stderr into /api/logs/stream
	// Bumps every spawned ffmpeg's own log level to "-v verbose" and prints
	// the full resolved command line before every spawn (not just on
	// failure) — independent of ffmpeg_debug_logs above, which only
	// controls whether stderr is streamed live vs just tail-captured.
	bool verbose_transcode_logs = false;
	int session_linger_secs     = 120; // keep session alive after last client disconnects
	// How far ahead (in seconds of content) a VOD session's main encoder is
	// allowed to run before being paused — see VodSession's lookahead monitor.
	int vod_lookahead_secs        = 180;
	int stream_buffer_size        = 1048576; // 1024 KB
	HwAccel hw_accel              = HwAccel::none;
	std::string vaapi_device      = "/dev/dri/renderD128";
	std::string default_logo_path = "/usr/local/share/hephaestus/assets/default_logo.png";
	// Bundled decode-probe sample clips (probe_h264.mp4/probe_hevc.mp4/
	// probe_av1.mp4) used by HwProbe::probeHwCapabilities() at startup.
	std::string hw_probe_assets_dir = "/usr/local/share/hephaestus/assets";
	// Root directory for HLS output (live channel tee output + VOD sessions).
	// Empty disables HLS entirely (legacy MPEG-TS-only behavior).
	std::string hls_root = "/tmp/hephaestus/hls";

	// In-memory segment cache byte budget, in MB. 0 (default) disables it
	// entirely — every low-power deployment that doesn't opt in pays zero
	// cost beyond a branch, see shared/cache/SegmentCache.h. See
	// CacheSizing.h for a startup-time suggestion computed from the
	// deployment's actual configured channels.
	size_t segment_cache_mb = 0;

	// HDHomeRun device identity presented to Plex / Emby / Jellyfin
	std::string hdhr_device_id = "48455048"; // "HEPH" in ASCII hex
	std::string hdhr_friendly  = "Hephaestus";
	int hdhr_tuner_count       = 4;

	// Hard ceiling on concurrent transcode sessions — each one is a spawned
	// ffmpeg-class process, so without a cap any authenticated caller
	// (including a free self-service guest account) could loop-request
	// sessions and exhaust the host's CPU/GPU. 0 disables the cap (opt-in,
	// for a trusted single-operator LAN deployment); every other deployment,
	// especially a public-facing one, should leave the default in place.
	int max_vod_sessions     = 8;
	int max_preview_sessions = 8;

	// Host-wide cap on concurrent hardware-encode (NVENC/VAAPI) sessions,
	// shared across channels/VOD/preview — see EncoderAdmission's own class
	// comment for why max_vod_sessions/max_preview_sessions above don't
	// already cover this (they cap concurrent *viewer* sessions per type,
	// with no cross-type coordination, and channels have no cap of their
	// own at all). 0 disables the cap (today's behavior, still the default —
	// this needs the deployment's real GPU's concurrent-session ceiling to
	// set correctly, which varies by card/driver, so it isn't safe to
	// default to a nonzero guess).
	int max_gpu_encode_sessions = 0;
};

inline HwAccel parseHwAccel(const std::string& s)
{
	if (s == "nvidia") return HwAccel::nvidia;
	if (s == "amd") return HwAccel::amd;
	return HwAccel::none;
}

inline Config parseConfig(int argc, char* argv[])
{
	Config cfg;
	for (int i = 1; i + 1 < argc; ++i)
	{
		std::string k = argv[i];
		std::string v = argv[i + 1];
		if (k == "--kairos-url")
		{
			cfg.kairos_url = v;
			++i;
		}
		else if (k == "--ffmpeg")
		{
			cfg.ffmpeg_path = v;
			++i;
		}
		else if (k == "--ffprobe")
		{
			cfg.ffprobe_path = v;
			++i;
		}
		else if (k == "--port")
		{
			cfg.port = std::stoi(v);
			++i;
		}
		else if (k == "--audio-lang")
		{
			cfg.audio_lang = v;
			++i;
		}
		else if (k == "--loudnorm")
		{
			cfg.loudnorm = (v != "0" && v != "false");
			++i;
		}
		else if (k == "--ffmpeg-debug")
		{
			cfg.ffmpeg_debug_logs = (v != "0" && v != "false");
			++i;
		}
		else if (k == "--verbose-transcode")
		{
			cfg.verbose_transcode_logs = (v != "0" && v != "false");
			++i;
		}
		else if (k == "--linger")
		{
			cfg.session_linger_secs = std::stoi(v);
			++i;
		}
		else if (k == "--vod-lookahead-secs")
		{
			cfg.vod_lookahead_secs = std::stoi(v);
			++i;
		}
		else if (k == "--buffer-size")
		{
			cfg.stream_buffer_size = std::stoi(v);
			++i;
		}
		else if (k == "--hw-accel")
		{
			cfg.hw_accel = parseHwAccel(v);
			++i;
		}
		else if (k == "--vaapi-device")
		{
			cfg.vaapi_device = v;
			++i;
		}
		else if (k == "--default-logo")
		{
			cfg.default_logo_path = v;
			++i;
		}
		else if (k == "--hw-probe-assets")
		{
			cfg.hw_probe_assets_dir = v;
			++i;
		}
		else if (k == "--hls-root")
		{
			cfg.hls_root = v;
			++i;
		}
		else if (k == "--segment-cache-mb")
		{
			cfg.segment_cache_mb = static_cast<size_t>(std::max(0, std::stoi(v)));
			++i;
		}
		else if (k == "--device-id")
		{
			cfg.hdhr_device_id = v;
			++i;
		}
		else if (k == "--friendly-name")
		{
			cfg.hdhr_friendly = v;
			++i;
		}
		else if (k == "--tuners")
		{
			cfg.hdhr_tuner_count = std::stoi(v);
			++i;
		}
	}
	if (auto* p = getenv("KAIROS_URL")) cfg.kairos_url = p;
	if (auto* p = getenv("HEPH_KAIROS_CONF_PATH")) cfg.kairos_conf_path = p;
	if (auto* p = getenv("FFMPEG_PATH")) cfg.ffmpeg_path = p;
	if (auto* p = getenv("FFPROBE_PATH")) cfg.ffprobe_path = p;
	if (auto* p = getenv("HEPH_LOUDNORM")) cfg.loudnorm = (std::string(p) != "0");
	if (auto* p = getenv("HEPH_FFMPEG_DEBUG")) cfg.ffmpeg_debug_logs = (std::string(p) != "0");
	if (auto* p = getenv("HEPH_VERBOSE_TRANSCODE")) cfg.verbose_transcode_logs = (std::string(p) != "0");
	if (auto* p = getenv("BUF_SIZE"))
	{
		int bs                 = std::stoi(p);
		cfg.stream_buffer_size = std::max(0, bs);
	}
	if (auto* p = getenv("HEPH_HW_ACCEL")) cfg.hw_accel = parseHwAccel(p);
	if (auto* p = getenv("HEPH_VAAPI_DEV")) cfg.vaapi_device = p;
	if (auto* p = getenv("HEPH_DEFAULT_LOGO")) cfg.default_logo_path = p;
	if (auto* p = getenv("HEPH_HLS_ROOT")) cfg.hls_root = p;
	if (auto* p = getenv("HEPH_SEGMENT_CACHE_MB")) cfg.segment_cache_mb = static_cast<size_t>(std::max(0, std::stoi(p)));
	if (auto* p = getenv("HEPH_VOD_LOOKAHEAD_SECS")) cfg.vod_lookahead_secs = std::stoi(p);
	if (auto* p = getenv("HEPH_HW_PROBE_ASSETS")) cfg.hw_probe_assets_dir = p;
	if (auto* p = getenv("HEPH_MAX_VOD_SESSIONS")) cfg.max_vod_sessions = std::stoi(p);
	if (auto* p = getenv("HEPH_MAX_PREVIEW_SESSIONS")) cfg.max_preview_sessions = std::stoi(p);
	if (auto* p = getenv("HEPH_MAX_GPU_ENCODE_SESSIONS")) cfg.max_gpu_encode_sessions = std::stoi(p);
	return cfg;
}