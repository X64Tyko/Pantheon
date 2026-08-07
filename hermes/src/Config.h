#pragma once
#include <string>
#include <cstdlib>
#include <algorithm>

struct Config
{
	std::string hephaestus_url = "http://localhost:8082";
	std::string kairos_url     = "http://localhost:8080";
	std::string hades_url      = "http://localhost:3000";
	int port                   = 8000;
	int linger_secs            = 30;

	// In-memory HLS segment cache budget, in MB. 0 (default) disables it
	// entirely — see shared/cache/SegmentCache.h. Independent of
	// Hephaestus's own HEPH_SEGMENT_CACHE_MB — they're separate processes
	// with separate budgets, each caching the same bytes for a different
	// reason (Hephaestus: skip its own disk read; Hermes: skip its own
	// upstream HTTP fetch to Hephaestus).
	size_t segment_cache_mb = 0;

	// HDHomeRun identity — Hermes owns this, Hephaestus should be internal-only
	std::string hdhr_device_id = "50414e54"; // "PANT" in ASCII hex
	std::string hdhr_friendly  = "Pantheon";
	int hdhr_tuner_count       = 4;

	// Same shared volume Hephaestus already reads kairos.conf's
	// internal_token from (see hephaestus/src/kairos/InternalToken.h) — used
	// by relayUpstreamLogs to authenticate its server-to-server pull of
	// Kairos's and Hephaestus's own /api/logs/stream, both now internal-
	// auth-gated. See docker-compose.yml: Hermes already mounts /data.
	std::string kairos_conf_path = "/data/kairos.conf";
};

inline Config parseConfig(int argc, char* argv[])
{
	Config cfg;
	for (int i = 1; i + 1 < argc; ++i)
	{
		std::string k = argv[i];
		std::string v = argv[i + 1];
		if (k == "--hephaestus-url")
		{
			cfg.hephaestus_url = v;
			++i;
		}
		else if (k == "--kairos-url")
		{
			cfg.kairos_url = v;
			++i;
		}
		else if (k == "--hades-url")
		{
			cfg.hades_url = v;
			++i;
		}
		else if (k == "--port")
		{
			cfg.port = std::stoi(v);
			++i;
		}
		else if (k == "--linger")
		{
			cfg.linger_secs = std::stoi(v);
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
	if (auto* p = getenv("HEPHAESTUS_URL")) cfg.hephaestus_url = p;
	if (auto* p = getenv("KAIROS_URL")) cfg.kairos_url = p;
	if (auto* p = getenv("HADES_URL")) cfg.hades_url = p;
	if (auto* p = getenv("HERMES_KAIROS_CONF_PATH")) cfg.kairos_conf_path = p;
	if (auto* p = getenv("HERMES_SEGMENT_CACHE_MB")) cfg.segment_cache_mb = static_cast<size_t>(std::max(0, std::stoi(p)));
	return cfg;
}