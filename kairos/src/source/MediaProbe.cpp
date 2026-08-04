#include "MediaProbe.h"
#include "log/DebugLog.h"
#include <cstdio>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>
}

using json = nlohmann::json;

namespace
{
	constexpr int64_t kMinMs = 1'000;      // 1 second
	constexpr int64_t kMaxMs = 86'400'000; // 24 hours
}                                          // namespace

bool durationLooksValid(int64_t ms) { return ms >= kMinMs && ms <= kMaxMs; }

namespace
{
	// Wrap s in single quotes, escaping any interior single quotes.
	// Still used by the ffprobe fallback paths below.
	std::string shellQuote(const std::string& s)
	{
		std::string r = "'";
		for (char c : s)
		{
			if (c == '\'') r += "'\\''";
			else r           += c;
		}
		return r + "'";
	}

	// ── libavformat RAII wrapper ──────────────────────────────────────────────

	struct FmtCtxDeleter
	{
		void operator()(AVFormatContext* p) const noexcept { avformat_close_input(&p); }
	};

	using FmtCtxPtr = std::unique_ptr<AVFormatContext, FmtCtxDeleter>;

	// Opens file_path with avformat_open_input and optionally calls
	// avformat_find_stream_info.  Returns a null unique_ptr on any error.
	// find_stream_info is needed for reliable codec identification on all
	// container types; for MP4/MKV it is cheap (data already in the header).
	FmtCtxPtr openFormat(const std::string& file_path, bool find_stream_info = true)
	{
		AVFormatContext* raw = nullptr;
		if (avformat_open_input(&raw, file_path.c_str(), nullptr, nullptr) < 0) return {};
		FmtCtxPtr ctx(raw);
		if (find_stream_info && avformat_find_stream_info(raw, nullptr) < 0) return {};
		return ctx;
	}

	// ── bit-depth helper (mirrors the ffprobe/json version) ──────────────────

	int bitDepthFromCodecpar(const AVCodecParameters* par)
	{
		if (par->bits_per_raw_sample > 0) return par->bits_per_raw_sample;
		if (par->format == AV_PIX_FMT_NONE) return 8;
		const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format));
		if (!desc || desc->nb_components == 0) return 8;
		return desc->comp[0].depth;
	}

	// ── duration from AVFormatContext ─────────────────────────────────────────

	// Returns 0 when the container has no valid duration field — callers can
	// then try longest per-stream duration as a fallback.
	int64_t fmtDurationMs(const AVFormatContext* ctx)
	{
		if (ctx->duration != AV_NOPTS_VALUE && ctx->duration > 0) return ctx->duration * 1000 / AV_TIME_BASE;
		return 0;
	}

	int64_t streamDurationMs(const AVStream* st)
	{
		if (st->duration != AV_NOPTS_VALUE && st->duration > 0) return av_rescale_q(st->duration, st->time_base, {1, 1000});
		return 0;
	}

	// ── ffprobe-based fallback duration probes ────────────────────────────────
	// These are only called when the native path fails entirely (e.g. the file
	// is a format avformat can't open, or the build environment lacks headers
	// for a specific container).

	int64_t probeFormatDurationMsFfprobe(const std::string& file_path)
	{
		const std::string cmd =
			"timeout -k 2 10 ffprobe -v quiet -show_entries format=duration -of csv=p=0 "
			+ shellQuote(file_path) + " 2>/dev/null";
		DLOG << "[probe] format-duration ffprobe cmd: " << cmd << '\n';
		FILE* pipe = popen(cmd.c_str(), "r");
		if (!pipe) return 0;
		char buf[64] = {};
		fgets(buf, sizeof(buf), pipe);
		pclose(pipe);
		try
		{
			const double secs = std::stod(buf);
			if (secs > 0.0) return static_cast<int64_t>(secs * 1000.0);
		}
		catch (...)
		{
		}
		return 0;
	}

	// ── native duration probe (used by validateDurationMs / probeDurationMs) ──

	int64_t probeDurationMsNative(const std::string& file_path)
	{
		auto ctx = openFormat(file_path, false); // chapters+streams are enough; no find_stream_info
		if (!ctx) return 0;
		int64_t dur = fmtDurationMs(ctx.get());
		if (dur > 0) return dur;
		// Per-stream fallback
		int64_t best = 0;
		for (unsigned i = 0; i < ctx->nb_streams; ++i)
		{
			int64_t ms = streamDurationMs(ctx->streams[i]);
			if (ms > best) best = ms;
		}
		return best;
	}

	int64_t probeDurationMs(const std::string& file_path)
	{
		const auto t0  = std::chrono::steady_clock::now();
		int64_t result = probeDurationMsNative(file_path);
		if (result > 0)
		{
			DLOG << "[probe] duration (native) " << elapsedMs(t0, std::chrono::steady_clock::now())
				<< "ms → " << result << "ms: " << file_path << '\n';
			return result;
		}
		result = probeFormatDurationMsFfprobe(file_path);
		DLOG << "[probe] duration (ffprobe fallback) " << elapsedMs(t0, std::chrono::steady_clock::now())
			<< "ms → " << result << "ms: " << file_path << '\n';
		return result;
	}

	// ── ffprobe-based keyframe fallback ───────────────────────────────────────
	// Used when the native stream index is empty (e.g. MKV with no Cues element,
	// or a container type whose demuxer doesn't pre-populate the seek index).

	std::vector<int64_t> probeKeyframesMsFfprobe(const std::string& file_path)
	{
		const std::string cmd = "timeout -k 2 30 ffprobe -v quiet -select_streams v:0 "
			"-show_entries packet=pts_time,flags -of csv=p=0 " + shellQuote(file_path) + " 2>/dev/null";
		DLOG << "[probe] keyframes ffprobe cmd: " << cmd << '\n';
		const auto t0 = std::chrono::steady_clock::now();
		FILE* pipe    = popen(cmd.c_str(), "r");
		std::vector<int64_t> keyframes;
		if (!pipe)
		{
			DLOG << "[probe] keyframes popen failed: " << file_path << '\n';
			return keyframes;
		}
		// Stream-parse line-by-line — do NOT buffer the full output into a
		// string first. For a 2-hour movie at 24fps this is ~170 000 lines;
		// buffering them all before parsing bloats RSS by hundreds of MB per
		// concurrent worker thread.
		char buf[256];
		while (fgets(buf, sizeof(buf), pipe))
		{
			std::string_view line(buf);
			while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
			auto comma = line.find(',');
			if (comma == std::string_view::npos || comma + 1 >= line.size()) continue;
			if (line[comma + 1] != 'K') continue;
			try
			{
				const double secs = std::stod(std::string(line.substr(0, comma)));
				keyframes.push_back(static_cast<int64_t>(secs * 1000.0 + 0.5));
			}
			catch (...)
			{
			}
		}
		pclose(pipe);
		DLOG << "[probe] keyframes (ffprobe fallback) done in "
			<< elapsedMs(t0, std::chrono::steady_clock::now())
			<< "ms → " << keyframes.size() << " keyframe(s): " << file_path << '\n';
		return keyframes;
	}

	// ── native keyframe extraction from stream seek index ─────────────────────
	// For MP4/MOV the index is always built from the stss+stts atoms in the moov
	// box (populated by avformat_open_input, no packet scan needed).  For MKV it
	// comes from the Cues element, which well-formed files always have.  Returns
	// empty when the demuxer reports zero index entries — caller falls back to
	// probeKeyframesMsFfprobe in that case.

	std::vector<int64_t> keyframesFromIndex(AVFormatContext* ctx, int stream_idx)
	{
		AVStream* st = ctx->streams[stream_idx];
		const int n  = avformat_index_get_entries_count(st);
		std::vector<int64_t> kf;
		kf.reserve(static_cast<size_t>(n > 0 ? n / 10 : 0)); // rough; most frames aren't keyframes
		for (int j = 0; j < n; ++j)
		{
			const AVIndexEntry* e = avformat_index_get_entry(st, j);
			if (!e) break;
			if (!(e->flags & AVINDEX_KEYFRAME)) continue;
			const int64_t ms = av_rescale_q(e->timestamp, st->time_base, {1, 1000});
			if (ms >= 0) kf.push_back(ms);
		}
		return kf;
	}

	// ── bit-depth from ffprobe JSON (kept for videoinfo ffprobe fallback) ─────
	int parseBitDepth(const json& s)
	{
		if (s.contains("bits_per_raw_sample"))
		{
			try
			{
				auto raw = s["bits_per_raw_sample"].get<std::string>();
				if (!raw.empty()) return std::stoi(raw);
			}
			catch (...)
			{
			}
		}
		std::string pix_fmt = s.value("pix_fmt", "");
		if (pix_fmt.find("10le") != std::string::npos || pix_fmt.find("10be") != std::string::npos) return 10;
		if (pix_fmt.find("12le") != std::string::npos || pix_fmt.find("12be") != std::string::npos) return 12;
		return 8;
	}
} // namespace

std::vector<Chapter> probeChapters(const std::string& file_path)
{
	DLOG << "[probe] chapters: " << file_path << '\n';
	const auto t0 = std::chrono::steady_clock::now();

	// Native path: read chapter list directly from the container header.
	// avformat_find_stream_info is not needed — chapters are available
	// immediately after avformat_open_input for both MP4 and MKV.
	auto ctx = openFormat(file_path, /*find_stream_info=*/false);
	if (ctx && ctx->nb_chapters > 0)
	{
		std::vector<Chapter> result;
		result.reserve(ctx->nb_chapters);
		for (unsigned i = 0; i < ctx->nb_chapters; ++i)
		{
			const AVChapter* ch = ctx->chapters[i];
			Chapter c;
			c.chapter_type             = "unclassified";
			c.source                   = "file";
			c.position                 = static_cast<int>(i);
			const AVDictionaryEntry* t = av_dict_get(ch->metadata, "title", nullptr, 0);
			if (t) c.title = t->value;
			c.start_ms = av_rescale_q(ch->start, ch->time_base, {1, 1000});
			c.end_ms   = av_rescale_q(ch->end, ch->time_base, {1, 1000});
			result.push_back(std::move(c));
		}
		DLOG << "[probe] chapters (native) done in " << elapsedMs(t0, std::chrono::steady_clock::now())
			<< "ms → " << result.size() << " chapter(s): " << file_path << '\n';
		return result;
	}
	// ctx == nullptr means avformat_open_input failed entirely; ctx with
	// nb_chapters == 0 is legitimately chapter-free — no ffprobe fallback needed.
	if (!ctx)
	{
		// avformat_open_input failed — fall back to ffprobe for exotic formats.
		const std::string cmd =
			"timeout -k 2 10 ffprobe -v quiet -print_format json -show_chapters "
			+ shellQuote(file_path) + " 2>/dev/null";
		DLOG << "[probe] chapters ffprobe fallback cmd: " << cmd << '\n';
		FILE* pipe = popen(cmd.c_str(), "r");
		if (!pipe)
		{
			DLOG << "[probe] chapters popen failed: " << file_path << '\n';
			return {};
		}
		std::string out;
		char buf[4096];
		while (fgets(buf, sizeof(buf), pipe)) out += buf;
		pclose(pipe);
		if (out.empty())
		{
			DLOG << "[probe] chapters (ffprobe fallback) done in " << elapsedMs(t0, std::chrono::steady_clock::now())
				<< "ms → no output: " << file_path << '\n';
			return {};
		}
		std::vector<Chapter> result;
		try
		{
			auto j = json::parse(out);
			if (!j.contains("chapters")) return result;
			int pos = 0;
			for (const auto& ch : j["chapters"])
			{
				Chapter c;
				c.chapter_type = "unclassified";
				c.source       = "file";
				c.position     = pos++;
				if (ch.contains("tags") && ch["tags"].is_object() && ch["tags"].contains("title")) c.title = ch["tags"]["title"].get<std::string>();
				if (ch.contains("start_time") && ch["start_time"].is_string())
				{
					try { c.start_ms = static_cast<int64_t>(std::stod(ch["start_time"].get<std::string>()) * 1000.0); }
					catch (...)
					{
					}
				}
				if (ch.contains("end_time") && ch["end_time"].is_string())
				{
					try { c.end_ms = static_cast<int64_t>(std::stod(ch["end_time"].get<std::string>()) * 1000.0); }
					catch (...)
					{
					}
				}
				result.push_back(std::move(c));
			}
		}
		catch (const std::exception& e)
		{
			std::cerr << "[probe] chapter parse error for " << file_path << ": " << e.what() << '\n';
		}
		DLOG << "[probe] chapters (ffprobe fallback) done in " << elapsedMs(t0, std::chrono::steady_clock::now())
			<< "ms → " << result.size() << " chapter(s): " << file_path << '\n';
		return result;
	}
	DLOG << "[probe] chapters done in " << elapsedMs(t0, std::chrono::steady_clock::now())
		<< "ms → 0 chapter(s): " << file_path << '\n';
	return {};
}

VideoInfo probeVideoInfo(const std::string& file_path)
{
	DLOG << "[probe] videoinfo: " << file_path << '\n';
	const auto t0 = std::chrono::steady_clock::now();

	// Native path.
	auto ctx = openFormat(file_path, /*find_stream_info=*/true);
	if (ctx)
	{
		VideoInfo result;
		for (unsigned i = 0; i < ctx->nb_streams; ++i)
		{
			const AVCodecParameters* par = ctx->streams[i]->codecpar;
			if (par->codec_type != AVMEDIA_TYPE_VIDEO) continue;
			result.width         = par->width;
			result.height        = par->height;
			result.bit_depth     = bitDepthFromCodecpar(par);
			const AVCodec* codec = avcodec_find_decoder(par->codec_id);
			if (codec) result.codec = codec->name;
			DLOG << "[probe] videoinfo (native) done in " << elapsedMs(t0, std::chrono::steady_clock::now())
				<< "ms → " << result.codec << " " << result.width << "x" << result.height
				<< " " << result.bit_depth << "-bit: " << file_path << '\n';
			return result;
		}
		// No video stream — return zero-valued struct without a fallback.
		DLOG << "[probe] videoinfo (native) done in " << elapsedMs(t0, std::chrono::steady_clock::now())
			<< "ms → no video stream: " << file_path << '\n';
		return result;
	}

	// avformat_open_input failed — ffprobe fallback.
	const std::string cmd =
		"timeout -k 2 15 ffprobe -v quiet -print_format json -show_streams "
		+ shellQuote(file_path) + " 2>/dev/null";
	DLOG << "[probe] videoinfo ffprobe fallback cmd: " << cmd << '\n';
	FILE* pipe = popen(cmd.c_str(), "r");
	if (!pipe)
	{
		DLOG << "[probe] videoinfo popen failed: " << file_path << '\n';
		return {};
	}
	std::string out;
	char buf[8192];
	while (fgets(buf, sizeof(buf), pipe)) out += buf;
	pclose(pipe);

	VideoInfo result;
	if (out.empty())
	{
		DLOG << "[probe] videoinfo (ffprobe fallback) done in " << elapsedMs(t0, std::chrono::steady_clock::now())
			<< "ms → no output: " << file_path << '\n';
		return result;
	}
	try
	{
		auto j = json::parse(out);
		if (j.contains("streams"))
		{
			for (const auto& s : j["streams"])
			{
				if (s.value("codec_type", "") != "video") continue;
				result.codec     = s.value("codec_name", "");
				result.width     = s.value("width", 0);
				result.height    = s.value("height", 0);
				result.bit_depth = parseBitDepth(s);
				break;
			}
		}
	}
	catch (const std::exception& e)
	{
		std::cerr << "[probe] videoinfo parse error for " << file_path << ": " << e.what() << '\n';
	}
	DLOG << "[probe] videoinfo (ffprobe fallback) done in " << elapsedMs(t0, std::chrono::steady_clock::now())
		<< "ms → " << result.codec << " " << result.width << "x" << result.height
		<< " " << result.bit_depth << "-bit: " << file_path << '\n';
	return result;
}

FileProbeInfo probeFileInfo(const std::string& file_path)
{
	DLOG << "[probe] fileinfo: " << file_path << '\n';
	const auto t0 = std::chrono::steady_clock::now();

	// ── Native path ───────────────────────────────────────────────────────────
	// Single avformat_open_input + avformat_find_stream_info call replaces
	// the two ffprobe subprocesses the old implementation used (one for
	// -show_format -show_streams, one for packet keyframe scan).
	auto ctx = openFormat(file_path, /*find_stream_info=*/true);
	if (ctx)
	{
		FileProbeInfo result;

		// Duration — format-level first (container header), then longest stream.
		result.duration_ms   = fmtDurationMs(ctx.get());
		int64_t best_stream  = 0;
		int video_stream_idx = -1;

		for (unsigned i = 0; i < ctx->nb_streams; ++i)
		{
			const AVStream* st           = ctx->streams[i];
			const AVCodecParameters* par = st->codecpar;

			if (result.duration_ms <= 0)
			{
				const int64_t sms = streamDurationMs(st);
				if (sms > best_stream) best_stream = sms;
			}

			if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx < 0)
			{
				video_stream_idx       = static_cast<int>(i);
				result.video.width     = par->width;
				result.video.height    = par->height;
				result.video.bit_depth = bitDepthFromCodecpar(par);
				const AVCodec* codec   = avcodec_find_decoder(par->codec_id);
				if (codec) result.video.codec = codec->name;
			}

			// Language tag — check both "language" (lowercase) and "LANGUAGE"
			// (uppercase, seen in some older MKV muxers).
			const AVDictionaryEntry* lang_e =
				av_dict_get(st->metadata, "language", nullptr, 0);
			if (!lang_e) lang_e = av_dict_get(st->metadata, "LANGUAGE", nullptr, 0);
			const std::string lang = lang_e ? lang_e->value : "";
			if (!lang.empty() && lang != "und")
			{
				if (par->codec_type == AVMEDIA_TYPE_AUDIO) result.langs.audio.push_back(lang);
				else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) result.langs.subtitle.push_back(lang);
			}
		}
		if (result.duration_ms <= 0) result.duration_ms = best_stream;

		// Keyframes — read directly from the seek index built by the demuxer.
		// For MP4 this is always populated from the stss/stts atoms in the moov
		// box.  For MKV it comes from the Cues element (present in all
		// well-formed files).  Empty index → fall back to the ffprobe packet scan
		// so we never silently produce an empty keyframe list for a valid file.
		if (video_stream_idx >= 0)
		{
			const auto t_kf     = std::chrono::steady_clock::now();
			result.keyframes_ms = keyframesFromIndex(ctx.get(), video_stream_idx);
			if (result.keyframes_ms.empty())
			{
				DLOG << "[probe] keyframe index empty, falling back to ffprobe scan: "
					<< file_path << '\n';
				result.keyframes_ms = probeKeyframesMsFfprobe(file_path);
			}
			else
			{
				DLOG << "[probe] keyframes (native index) done in "
					<< elapsedMs(t_kf, std::chrono::steady_clock::now())
					<< "ms → " << result.keyframes_ms.size() << " keyframe(s): "
					<< file_path << '\n';
			}
		}

		DLOG << "[probe] fileinfo (native) done in " << elapsedMs(t0, std::chrono::steady_clock::now())
			<< "ms → duration=" << result.duration_ms
			<< "ms " << result.video.width << "x" << result.video.height
			<< " audio=" << result.langs.audio.size()
			<< " subtitle=" << result.langs.subtitle.size()
			<< " keyframes=" << result.keyframes_ms.size()
			<< ": " << file_path << '\n';
		return result;
	}

	// ── ffprobe fallback (avformat_open_input failed — exotic container) ──────
	DLOG << "[probe] fileinfo: avformat_open_input failed, falling back to ffprobe: "
		<< file_path << '\n';
	const std::string cmd =
		"timeout -k 2 15 ffprobe -v quiet -print_format json -show_format -show_streams "
		+ shellQuote(file_path) + " 2>/dev/null";
	DLOG << "[probe] fileinfo ffprobe fallback cmd: " << cmd << '\n';
	FILE* pipe = popen(cmd.c_str(), "r");
	FileProbeInfo result;
	if (!pipe)
	{
		DLOG << "[probe] fileinfo popen failed: " << file_path << '\n';
		return result;
	}
	std::string out;
	char buf[8192];
	while (fgets(buf, sizeof(buf), pipe)) out += buf;
	pclose(pipe);
	if (out.empty())
	{
		DLOG << "[probe] fileinfo (ffprobe fallback) done in " << elapsedMs(t0, std::chrono::steady_clock::now())
			<< "ms → no output: " << file_path << '\n';
		return result;
	}
	try
	{
		auto j = json::parse(out);
		if (j.contains("format") && j["format"].contains("duration"))
		{
			try
			{
				const double secs = std::stod(j["format"]["duration"].get<std::string>());
				if (secs > 0.0) result.duration_ms = static_cast<int64_t>(secs * 1000.0);
			}
			catch (...)
			{
			}
		}
		if (j.contains("streams"))
		{
			int64_t longest_stream_ms = 0;
			bool got_video            = false;
			for (const auto& s : j["streams"])
			{
				const std::string type = s.value("codec_type", "");
				if (result.duration_ms <= 0 && s.contains("duration"))
				{
					try
					{
						const double secs = std::stod(s["duration"].get<std::string>());
						const int64_t sms = static_cast<int64_t>(secs * 1000.0);
						if (sms > longest_stream_ms) longest_stream_ms = sms;
					}
					catch (...)
					{
					}
				}
				if (type == "video" && !got_video)
				{
					result.video.codec     = s.value("codec_name", "");
					result.video.width     = s.value("width", 0);
					result.video.height    = s.value("height", 0);
					result.video.bit_depth = parseBitDepth(s);
					got_video              = true;
				}
				std::string lang;
				if (s.contains("tags") && s["tags"].is_object())
				{
					auto& t = s["tags"];
					if (t.contains("language")) lang = t["language"].get<std::string>();
					else if (t.contains("LANGUAGE")) lang = t["LANGUAGE"].get<std::string>();
				}
				if (!lang.empty() && lang != "und")
				{
					if (type == "audio") result.langs.audio.push_back(lang);
					else if (type == "subtitle") result.langs.subtitle.push_back(lang);
				}
			}
			if (result.duration_ms <= 0) result.duration_ms = longest_stream_ms;
		}
	}
	catch (const std::exception& e)
	{
		std::cerr << "[probe] fileinfo parse error for " << file_path << ": " << e.what() << '\n';
	}
	result.keyframes_ms = probeKeyframesMsFfprobe(file_path);
	DLOG << "[probe] fileinfo (ffprobe fallback) done in " << elapsedMs(t0, std::chrono::steady_clock::now())
		<< "ms → duration=" << result.duration_ms
		<< "ms " << result.video.width << "x" << result.video.height
		<< " audio=" << result.langs.audio.size() << " subtitle=" << result.langs.subtitle.size()
		<< ": " << file_path << '\n';
	return result;
}

std::string bucketResolutionLabel(int height)
{
	if (height >= 2000) return "4K";
	if (height >= 900) return "1080p";
	if (height >= 600) return "720p";
	if (height > 0) return "SD";
	return "";
}

int64_t validateDurationMs(int64_t dur, const std::string& file_path)
{
	if (durationLooksValid(dur)) return dur;

	if (dur != 0)
	{
		std::cerr << "[probe] out-of-range duration_ms=" << dur
			<< " for " << file_path << " — probing with ffprobe\n";
	}

	const int64_t probed = probeDurationMs(file_path);
	if (durationLooksValid(probed))
	{
		std::cout << "[probe] " << file_path
			<< " — probed duration: " << probed << " ms\n";
		return probed;
	}

	std::cerr << "[probe] WARNING: could not determine valid duration for "
		<< file_path << " (source=" << dur
		<< " probed=" << probed << ")\n";
	return 0;
}