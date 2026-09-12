#include "MediaNormalizer.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using json = nlohmann::json;

namespace
{
	// Codebase convention (ChapterDetector.cpp, MediaProbe.cpp): each
	// shell-out module keeps its own copy rather than sharing one.
	std::string shellQuote(const std::string& s)
	{
		std::string r = "'";
		for (char c : s) r += (c == '\'') ? "'\\''" : std::string(1, c);
		return r + "'";
	}

	double parseFrameRateFraction(const std::string& s)
	{
		auto slash = s.find('/');
		try
		{
			if (slash == std::string::npos) return std::stod(s);
			double num = std::stod(s.substr(0, slash));
			double den = std::stod(s.substr(slash + 1));
			return den > 0.0 ? num / den : 0.0;
		}
		catch (...) { return 0.0; }
	}

	constexpr int kProbeTimeoutSecs  = 15;
	constexpr int kEncodeTimeoutSecs = 6 * 3600; // archival re-encode, not realtime-bounded
	// Duration tolerance for verifyNormalizedOutput: generous enough to
	// absorb container-level rounding, tight enough to catch a truncated
	// encode (killed mid-run, disk full, etc).
	constexpr int64_t kDurationToleranceMinMs = 2000;
	constexpr double kDurationTolerancePct    = 0.01;
}

CodecSummary probeCodecSummary(const std::string& file_path)
{
	CodecSummary result;
	const std::string cmd = "timeout -k 5 " + std::to_string(kProbeTimeoutSecs) +
		" ffprobe -v quiet -print_format json -show_streams -show_format " +
		shellQuote(file_path) + " 2>/dev/null";

	FILE* pipe = popen(cmd.c_str(), "r");
	if (!pipe) return result;
	std::string out;
	char buf[8192];
	while (fgets(buf, sizeof(buf), pipe)) out += buf;
	pclose(pipe);
	if (out.empty()) return result;

	try
	{
		auto j = json::parse(out);
		if (j.contains("streams"))
		{
			for (const auto& s : j["streams"])
			{
				const std::string type = s.value("codec_type", "");
				if (type == "video" && !result.has_video)
				{
					result.has_video    = true;
					result.video_codec  = s.value("codec_name", "");
					result.r_frame_rate = s.value("r_frame_rate", "");
					double r_fps        = parseFrameRateFraction(result.r_frame_rate);
					double avg_fps      = parseFrameRateFraction(s.value("avg_frame_rate", ""));
					result.likely_vfr   = r_fps > 0.0 && avg_fps > 0.0 && std::abs(r_fps - avg_fps) > 0.05;
				}
				else if (type == "audio" && result.audio_codec.empty())
				{
					result.audio_codec = s.value("codec_name", "");
				}
			}
		}
		if (j.contains("format") && j["format"].contains("duration"))
		{
			try { result.duration_ms = static_cast<int64_t>(std::stod(j["format"]["duration"].get<std::string>()) * 1000.0); }
			catch (...)
			{
			}
		}
	}
	catch (...)
	{
	}
	return result;
}

bool needsNormalize(const CodecSummary& s)
{
	if (!s.has_video) return false;
	return s.video_codec != "h264" || s.audio_codec != "aac" || s.likely_vfr;
}

bool runNormalizeEncode(const std::string& file_path, const std::string& tmp_path,
						const std::string& r_frame_rate)
{
	std::string cmd = "timeout -k 30 " + std::to_string(kEncodeTimeoutSecs) + " ffmpeg -y -i " +
		shellQuote(file_path) +
		" -map 0:v:0 -map 0:a? -map 0:s? -map_chapters 0"
		" -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p -vsync cfr";
	if (!r_frame_rate.empty()) cmd += " -r " + r_frame_rate;
	cmd += " -c:a aac -c:s copy " + shellQuote(tmp_path) + " >/dev/null 2>&1";
	return std::system(cmd.c_str()) == 0;
}

bool verifyNormalizedOutput(const std::string& tmp_path, int64_t expected_duration_ms)
{
	CodecSummary s = probeCodecSummary(tmp_path);
	if (!s.has_video || s.video_codec != "h264" || s.audio_codec != "aac") return false;
	if (expected_duration_ms <= 0) return true;

	int64_t tolerance = std::max<int64_t>(kDurationToleranceMinMs,
										  static_cast<int64_t>(expected_duration_ms * kDurationTolerancePct));
	return std::llabs(s.duration_ms - expected_duration_ms) <= tolerance;
}