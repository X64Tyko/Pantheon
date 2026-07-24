#include "VodSession.h"
#include "EncoderArgs.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <unordered_map>

static int64_t nowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

// Shared by buildVodArgs' -hls_time, pushVideoEncoderArgs' -force_key_frames,
// and this file's own playlist synthesis — see the matching constant/comment
// in ChannelSession.cpp for why these must stay in sync (HLS can only cut a
// segment at a keyframe).
static constexpr int kVodHlsSegmentSecs = 6;

// How often the lookahead monitor re-checks generated-segment progress and
// the pause/resume decision.
static constexpr int kLookaheadTickSecs = 2;
// How close (in segments) the viewer needs to be to the generated frontier
// before a request there is treated as "just wait a moment" rather than
// "restart the encoder here" — roughly 24s of slack at the default 6s
// segment length.
static constexpr int kVodCatchUpMarginSegments = 4;

// Replicates ffmpeg's own -hls_time cutting rule for a direct-play (stream
// copy) session: cut at the first keyframe at or after hls_time_secs seconds
// have elapsed since the last cut. Given the file's real keyframe timestamps
// (MediaProbe::probeKeyframeTimestampsMs), this predicts EXACTLY where a
// -c:v copy invocation will actually cut segments, since stream copy can't
// force keyframes onto any other cadence — the assumed-uniform-cadence
// approach only holds for transcode/burn-in, where -force_key_frames
// controls placement directly. Returns an empty vector if keyframes_ms is
// empty (probe failed) — callers fall back to the uniform assumption.
static std::vector<int64_t> simulateDirectPlaySegmentBoundaries(
    const std::vector<int64_t>& keyframes_ms, int hls_time_secs)
{
    std::vector<int64_t> bounds;
    if (keyframes_ms.empty()) return bounds;
    const int64_t hls_time_ms = int64_t(hls_time_secs) * 1000;
    int64_t current_start = keyframes_ms.front(); // should be ~0
    bounds.push_back(current_start);
    int64_t target = current_start + hls_time_ms;
    for (int64_t kf : keyframes_ms) {
        if (kf <= current_start) continue;
        if (kf >= target) {
            bounds.push_back(kf);
            current_start = kf;
            target = current_start + hls_time_ms;
        }
    }
    return bounds;
}

// Text-based subtitle codecs ffmpeg can transcode to WebVTT for an HLS
// sidecar track.
static bool isTextSubtitleCodec(const std::string& codec) {
	return codec == "subrip" || codec == "ass" || codec == "ssa" ||
		   codec == "mov_text" || codec == "webvtt" || codec == "text";
}

// Rough BANDWIDTH estimate for the master manifest's #EXT-X-STREAM-INF — HLS
// spec requires this attribute but doesn't require it to be exact, and this
// codebase doesn't track actual encoded bitrate anywhere (VBR/CRF-driven,
// stream-copy for direct-play). A resolution-based ladder guess plus the
// fixed 192kbps audio bitrate (pushAudioEncoderArgs) is good enough for
// clients that only use it for initial ABR ordering — this manifest only
// ever has the one variant anyway.
static int64_t estimateBandwidthBps(const MediaInfo& info) {
	int height = info.video.empty() ? 0 : info.video[0].height;
	int64_t video_bps = height >= 2000 ? 20000000 : height >= 1000 ? 8000000 : height >= 700 ? 4000000 : 2000000;
	return video_bps + 192000;
}

// Bitmap/graphic subtitle formats (Blu-ray/DVD/DVB) have no text to extract
// into a sidecar — burn them directly into the video via ffmpeg's overlay
// filter instead (see buildVodArgs' subtitleBurnIn branch). Mutually
// exclusive with isTextSubtitleCodec.
static bool isBitmapSubtitleCodec(const std::string& codec) {
	return codec == "hdmv_pgs_subtitle" || codec == "dvd_subtitle" || codec == "dvb_subtitle";
}

// pickSubtitleTrack (MediaProbe) only searches embedded tracks — a saved
// preference also needs to match an external sidecar (e.g. Owl House, whose
// subtitles are entirely external), returning the same negative-index
// encoding start()'s subtitle_track<=-2 branch expects. Non-forced preferred
// over forced when both match, since a saved "watch with English subs"
// preference means the full dialogue track, not just a forced/signs one.
static int pickExternalSubtitleTrack(const std::vector<ExternalSubtitle>& external_subtitles,
									  const std::string& preferred_lang) {
	if (preferred_lang.empty()) return -1;
	int forced_match = -1;
	for (size_t i = 0; i < external_subtitles.size(); ++i) {
		if (external_subtitles[i].language.substr(0, 3) != preferred_lang.substr(0, 3)) continue;
		if (!external_subtitles[i].forced) return -static_cast<int>(i) - 2;
		if (forced_match == -1) forced_match = -static_cast<int>(i) - 2;
	}
	return forced_match;
}

// h264/aac is the conservative "every browser can play this without
// transcoding" fallback for a client that hasn't declared its own decode
// capability (see ClientCapabilities.h). When it has, direct-play is
// decided against that declared set instead — native Android via
// MediaCodec, or a real TV's hardware decoder, can often handle far more
// than a browser's <video>/hls.js path can (hevc, av1, ac3, ...), and a
// fixed global allowlist has no way to take advantage of that.
static bool isDirectPlayable(const MediaInfo& info, int audioTrack,
							  const std::optional<ClientCapabilities>& client_caps) {
	if (info.video.empty()) return false;
	auto it = std::find_if(info.audio.begin(), info.audio.end(),
		[&](const AudioTrack& t) { return t.relative_index == audioTrack; });
	if (it == info.audio.end()) return false;

	const std::string& videoCodec = info.video[0].codec;
	const std::string& audioCodec = it->codec;
	if (client_caps)
		return client_caps->video_codecs.count(videoCodec) > 0 && client_caps->audio_codecs.count(audioCodec) > 0;
	return videoCodec == "h264" && audioCodec == "aac";
}

static std::vector<std::string> buildVodArgs(
	const std::string& ffmpeg_path,
	const std::string& file_path,
	int64_t positionMs,
	int audioTrack,
	int subtitleTrack,
	bool directPlay,
	bool subtitleBurnIn,
	HwAccel hw_accel,
	const std::string& vaapi_device,
	HwAccel decode_hw_accel,
	const std::set<std::string>& decodable_codecs,
	const std::string& source_codec,
	const VideoTrack* source_video,
	bool hdr_capable,
	bool verbose_transcode_logs,
	const std::string& dir,
	int hlsStartNumber)
{
	std::vector<std::string> a;
	a.push_back(ffmpeg_path);
	pushLogLevelArgs(a, verbose_transcode_logs);

	a.insert(a.end(), {"-fflags", "+genpts+discardcorrupt"});

	pushVaapiDeviceArg(a, hw_accel, decode_hw_accel, vaapi_device);

	if (positionMs > 0) {
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(3) << (positionMs / 1000.0);
		a.push_back("-ss"); a.push_back(ss.str());
	}

	// Direct-play is a pure stream copy — nothing gets decoded, so a decode
	// hwaccel would be a pointless no-op at best.
	if (!directPlay) pushHwAccelDecodeArgs(a, decode_hw_accel, decodable_codecs, source_codec);

	a.push_back("-i"); a.push_back(file_path);

	if (directPlay) {
		a.insert(a.end(), {"-map", "0:v:0?", "-map", "0:a:" + std::to_string(audioTrack) + "?"});
		a.insert(a.end(), {"-dn", "-map_chapters", "-1"});
		a.insert(a.end(), {"-c:v", "copy", "-c:a", "copy"});
	} else if (subtitleBurnIn) {
		// Bitmap subtitles (PGS/DVD/DVB) have no text to extract into a
		// sidecar, so composite them directly onto the video instead —
		// overlay needs two explicit inputs (video + subtitle stream),
		// which -vf's single-input shorthand can't express, so this builds
		// an equivalent -filter_complex graph rather than the usual
		// -map 0:v:0? + -vf chain. ffmpeg decodes bitmap subtitle streams as
		// a sequence of timed, transparent-background image frames —
		// overlay composites them onto matching video frames directly, no
		// OCR/text extraction involved. Any other video filters (scale,
		// AMD's hwupload) chain after the overlay in the same linear graph.
		std::vector<std::string> vfParts;
		pushVideoEncoderArgs(a, vfParts, hw_accel, kVodHlsSegmentSecs, source_video, hdr_capable);
		std::string filterComplex = "[0:v:0][0:s:" + std::to_string(subtitleTrack) + "]overlay";
		for (auto& p : vfParts) filterComplex += "," + p;
		filterComplex += "[vout]";
		a.insert(a.end(), {"-filter_complex", filterComplex});
		a.insert(a.end(), {"-map", "[vout]", "-map", "0:a:" + std::to_string(audioTrack) + "?"});
		a.insert(a.end(), {"-dn", "-map_chapters", "-1"});
		pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, /*audio_bitrate_kbps=*/192);
	} else {
		a.insert(a.end(), {"-map", "0:v:0?", "-map", "0:a:" + std::to_string(audioTrack) + "?"});
		a.insert(a.end(), {"-dn", "-map_chapters", "-1"});
		std::vector<std::string> vfParts;
		pushVideoEncoderArgs(a, vfParts, hw_accel, kVodHlsSegmentSecs, source_video, hdr_capable);
		pushVideoFilterArgs(a, vfParts);
		pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, /*audio_bitrate_kbps=*/192);
	}

	// "vod" playlist type (not "event" as this used to be): ffmpeg's own
	// playlist output here is now purely a private scratch file Hephaestus
	// itself never serves — VodSession synthesizes and serves the real,
	// complete, #EXT-X-ENDLIST-terminated playlist.m3u8 directly (see
	// buildStaticPlaylist()), so the old "does ffmpeg's own HLS muxer emit
	// the playlist incrementally or hold it back" distinction that used to
	// matter here no longer applies to anything a client ever sees.
	// -start_number (not -hls_start_number_source) keeps this invocation's segment numbering aligned
	// with the position restartAt() is spawning it at — every spawn goes
	// through restartAt(), which is what guarantees the two never drift.
	a.insert(a.end(), {
		"-f", "hls",
		"-hls_time", std::to_string(kVodHlsSegmentSecs),
		"-hls_playlist_type", "vod",
		"-hls_list_size", "0",
		"-start_number", std::to_string(hlsStartNumber),
		"-hls_segment_filename", dir + "/seg-%05d.ts",
		dir + "/encoder.m3u8"
	});

	return a;
}

// SRT files carry no charset declaration; ffmpeg's srt demuxer assumes UTF-8
// with no fallback. Non-English sidecars are often Windows-1252 instead
// (English ones are usually plain ASCII, which is valid UTF-8 by accident).
static bool looksLikeValidUtf8(const std::string& bytes) {
	size_t i = 0;
	while (i < bytes.size()) {
		unsigned char c = static_cast<unsigned char>(bytes[i]);
		int extra;
		if (c < 0x80) extra = 0;
		else if ((c & 0xE0) == 0xC0) extra = 1;
		else if ((c & 0xF0) == 0xE0) extra = 2;
		else if ((c & 0xF8) == 0xF0) extra = 3;
		else return false; // invalid leading byte
		if (i + static_cast<size_t>(extra) >= bytes.size()) break; // truncated at buffer end — inconclusive, don't fail on it
		for (int k = 1; k <= extra; ++k) {
			unsigned char cc = static_cast<unsigned char>(bytes[i + static_cast<size_t>(k)]);
			if ((cc & 0xC0) != 0x80) return false;
		}
		i += static_cast<size_t>(extra) + 1;
	}
	return true;
}

// "" (ffmpeg's own default) if the sidecar looks like valid UTF-8 or can't
// be read; "CP1252" otherwise — covers most legacy Western-language .srt files.
static std::string sniffSubCharenc(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return "";
	std::string buf(65536, '\0');
	f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
	buf.resize(static_cast<size_t>(f.gcount()));
	return looksLikeValidUtf8(buf) ? "" : "CP1252";
}

// The independent, whole-file subtitle extraction process — see VodSession.h's
// class comment for why this is a second, separate FfmpegProcess rather than
// a second output group tacked onto the main encoder's own invocation. No
// -ss: unlike the old position-onward extraction, this always covers the
// entire file, since a backward seek under the new sliding-window model can
// reach any part of it over the session's life, and a sideloaded subtitle
// track is only ever fetched once by the player.
static std::vector<std::string> buildVodSubtitleArgs(
	const std::string& ffmpeg_path,
	const std::string& file_path,
	int subtitleTrack,
	const std::string& externalSubtitlePath,
	bool verbose_transcode_logs,
	const std::string& outPath,
	bool on_fly = false)
{
	std::vector<std::string> a;
	a.push_back(ffmpeg_path);
	pushLogLevelArgs(a, verbose_transcode_logs);
	// pull out embeded subtitle
	if (externalSubtitlePath.empty())
	{
		a.push_back("-i");
		a.push_back(file_path);
		a.insert(a.end(), {"-map", "0:s:" + std::to_string(subtitleTrack)});
	}
	else
	{
		auto charenc = sniffSubCharenc(externalSubtitlePath);
		a.insert(a.end(),{"-sub_charenc", charenc.empty() ? "utf-8" : charenc, "-i"});
		a.push_back(externalSubtitlePath);
		a.insert(a.end(), {"-map", "0:s:0"});
	}
	
	if (on_fly)
		a.insert(a.end(), {"-f", "webvtt", "pipe:1"});
	else
		a.insert(a.end(), {"-c:s", "webvtt", outPath});
	return a;
}

VodSession::VodSession(std::string session_id, std::string ffmpeg_path, VodStreamOptions opts)
	: session_id(std::move(session_id)), ffmpeg_path(std::move(ffmpeg_path)), opts(std::move(opts)) {}

VodSession::~VodSession() { stop(); }

bool VodSession::start(const std::string& file_path, int64_t position_ms,
						int audio_track, int subtitle_track, bool hdr_capable,
						const std::optional<ClientCapabilities>& client_caps,
						const std::vector<ExternalSubtitle>& external_subtitles,
						int64_t fallback_duration_ms,
						const std::string& preferred_audio_lang,
						const std::string& preferred_subtitle_lang) {
	external_subtitles_ = external_subtitles;
	auto info = probeMediaCached(opts.ffprobe_path, file_path);
	if (!info) {
		std::cerr << "[vod:" << session_id << "] probe failed for \"" << file_path << "\"\n";
		return false;
	}
	media_info = *info;
	this->file_path = file_path;
	started_at_ms   = nowMs();
	source_codec_ = media_info.video.empty() ? "" :
		decodeCodecKey(media_info.video[0].codec, media_info.video[0].bit_depth);

	if (audio_track < 0) audio_track = pickAudioTrack(media_info, preferred_audio_lang);
	// Only auto-pick a subtitle when the client left it fully unset (-1) —
	// -1 is also "explicitly off", but there's no separate wire value for
	// "off" vs "never chosen," so a saved preference always wins over -1.
	// Embedded tracks take priority; falls back to an external sidecar match
	// (see pickExternalSubtitleTrack) since either helper no-ops when
	// preferred_subtitle_lang is empty.
	if (subtitle_track == -1) {
		subtitle_track = pickSubtitleTrack(media_info, preferred_subtitle_lang);
		if (subtitle_track == -1) subtitle_track = pickExternalSubtitleTrack(external_subtitles, preferred_subtitle_lang);
	}
	audio_track_    = audio_track;
	subtitle_track_ = subtitle_track;
	hdr_capable_    = hdr_capable;
	client_caps_    = client_caps;
	direct_play = isDirectPlayable(media_info, audio_track, client_caps);

	{
		auto audioIt = std::find_if(media_info.audio.begin(), media_info.audio.end(),
			[&](const AudioTrack& t) { return t.relative_index == audio_track; });
		std::string sourceVideoCodec = media_info.video.empty() ? "(none)" : media_info.video[0].codec;
		std::string sourceAudioCodec = audioIt != media_info.audio.end() ? audioIt->codec : "(none)";
		std::cerr << "[vod:" << session_id << "] source video=" << sourceVideoCodec
				  << " audio=" << sourceAudioCodec;
		if (client_caps) {
			std::cerr << " | client declared video=[";
			bool first = true;
			for (auto& c : client_caps->video_codecs) { if (!first) std::cerr << ","; std::cerr << c; first = false; }
			std::cerr << "] audio=[";
			first = true;
			for (auto& c : client_caps->audio_codecs) { if (!first) std::cerr << ","; std::cerr << c; first = false; }
			std::cerr << "]";
		} else {
			std::cerr << " | no client capability declaration cached — falling back to h264/aac allowlist";
		}
		std::cerr << " -> direct_play=" << (direct_play ? "yes" : "no") << "\n";
	}

	auto resolved = resolveSubtitleTrack(subtitle_track);
	subtitle_output          = resolved.output;
	subtitle_burn_in         = resolved.burn_in;
	external_subtitle_path_  = resolved.external_path;
	// Burning a subtitle onto the video means decoding and re-encoding it —
	// direct play (a pure stream copy) is incompatible with that.
	if (subtitle_burn_in) direct_play = false;

	auto d = dir();
	std::error_code ec;
	std::filesystem::create_directories(d, ec);
	if (ec) {
		std::cerr << "[vod:" << session_id << "] failed to create \"" << d << "\": " << ec.message() << "\n";
		return false;
	}

	effective_duration_ms = media_info.duration_ms > 0 ? media_info.duration_ms : fallback_duration_ms;
	if (effective_duration_ms <= 0) {
		std::cerr << "[vod:" << session_id << "] no duration available (own probe and fallback both empty) — refusing to start\n";
		return false;
	}

	computeSegmentBoundaries();

	// Not spawned eagerly here anymore — see spawnSubtitleProcess()'s own
	// comment. The /subs.vtt route triggers it lazily on first request.

	active = true;
	touch();

	// Which segment covers position_ms — segment_start_ms is ascending, so
	// the last entry not exceeding position_ms is the one. total_segments is
	// small enough (hundreds at most, even for a very long file) that a
	// linear scan done once at session start is not worth a binary search.
	int start_segment = 0;
	for (size_t i = 0; i < segment_start_ms.size(); ++i) {
		if (segment_start_ms[i] <= position_ms) start_segment = static_cast<int>(i);
		else break;
	}
	//initial_start_segment_ = start_segment;
	//initial_target_reached.store(false);
	if (!restartAt(start_segment)) {
		active = false;
		return false;
	}

	lookahead_stop = false;
	lookahead_thread = std::thread([this] { lookaheadLoop(); });

	std::cerr << "[vod:" << session_id << "] started: \"" << file_path << "\""
			  << " duration=" << effective_duration_ms << "ms segments=" << total_segments
			  << " direct_play=" << (direct_play ? "yes" : "no") << "\n";
	return true;
}

void VodSession::computeSegmentBoundaries() {
	segment_start_ms.clear();
	if (direct_play) {
		// Stream copy can only cut where a real keyframe already exists —
		// probe them and predict ffmpeg's own exact cut points rather than
		// assuming a uniform cadence the encoder has no way to honor.
		auto keyframes = probeKeyframeTimestampsMs(opts.ffprobe_path, file_path);
		segment_start_ms = simulateDirectPlaySegmentBoundaries(keyframes, kVodHlsSegmentSecs);
		if (segment_start_ms.empty())
			std::cerr << "[vod:" << session_id << "] keyframe probe failed/empty for direct-play — "
			             "falling back to assumed uniform segment cadence (segment boundaries may drift from actual cut points)\n";
	}
	if (segment_start_ms.empty()) {
		// Transcode/burn-in (keyframes forced at exactly this cadence via
		// -force_key_frames) or a failed direct-play keyframe probe.
		int n = static_cast<int>(std::ceil(double(effective_duration_ms) / (kVodHlsSegmentSecs * 1000.0)));
		if (n < 1) n = 1;
		for (int i = 0; i < n; ++i) segment_start_ms.push_back(int64_t(i) * kVodHlsSegmentSecs * 1000);
	}
	total_segments = static_cast<int>(segment_start_ms.size());
}

VodSession::SubtitleResolution VodSession::resolveSubtitleTrack(int track) const {
	SubtitleResolution r;
	if (track <= -2) {
		// Negative-index scheme for external sidecar files — see the
		// header's start() comment. Always text, never burn-in, and never
		// affects direct-play (no decode of video/audio involved).
		const size_t idx = static_cast<size_t>(-track - 2);
		if (idx < external_subtitles_.size()) {
			r.external_path = external_subtitles_[idx].file_path;
			r.output = true;
		}
	} else if (track >= 0) {
		auto it = std::find_if(media_info.subtitles.begin(), media_info.subtitles.end(),
			[&](const SubtitleTrack& t) { return t.relative_index == track; });
		if (it != media_info.subtitles.end()) {
			r.output  = isTextSubtitleCodec(it->codec);
			r.burn_in = isBitmapSubtitleCodec(it->codec);
		}
	}
	return r;
}

bool VodSession::ensureAudioTrack(int track) {
	{
		std::lock_guard<std::mutex> lock(session_mtx);
		if (!active.load()) return false;
		if (track == audio_track_) {
			std::cerr << "[vod:" << session_id << "] ensureAudioTrack track=" << track
					  << " already active — no-op\n";
			return true;
		}
		std::cerr << "[vod:" << session_id << "] ensureAudioTrack SWITCHING audio_track_ " << audio_track_
				  << " -> " << track << " (will restart encoder)\n";
		audio_track_ = track;
		// Different audio tracks can differ in codec (e.g. a DTS/AC3
		// commentary track alongside a default AAC one) — direct-play
		// eligibility, and therefore the segment table, can flip on a
		// switch. subtitle_burn_in's own direct-play override (see start())
		// stays in force regardless of the audio codec.
		bool new_direct_play = subtitle_burn_in ? false : isDirectPlayable(media_info, audio_track_, client_caps_);
		if (new_direct_play != direct_play) {
			direct_play = new_direct_play;
			computeSegmentBoundaries();
		}
	}
	// restartAt() takes session_mtx itself — must not still hold it here.
	int target_segment = std::clamp(last_requested_segment.load(), 0, total_segments - 1);
	return restartAt(target_segment);
}

// RFC 6381 codec-string derivation for the master playlist's CODECS
// attribute (see buildMasterPlaylist below). Only covers what's actually
// reachable from ffprobe's own reported fields and is common enough to be
// worth the risk — h264 video, and the handful of audio codecs this
// codebase's decodable-codec allowlists actually let through. Deliberately
// returns nullopt (rather than a best-effort guess) for anything else:
// declaring a wrong CODECS string is worse than omitting it entirely (a
// strict player can reject the whole manifest), and this is the same "don't
// guess wrong" call the old no-CODECS comment made, just narrowed to the
// codecs it's actually safe to be confident about.
static std::optional<std::string> h264CodecString(const VideoTrack& v) {
	// profile_idc byte, keyed off ffprobe's human-readable profile name —
	// standard H.264 Annex A profile_idc values. Constrained/Baseline
	// variants collapse onto the same profile_idc (0x42); the
	// constraint-flags byte that would normally distinguish them is set to
	// 0x00 below, a widely-used simplification (hls.js/shaka-player do the
	// same) that strict decoders tolerate fine since it only affects
	// negotiation, not actual decoding.
	static const std::unordered_map<std::string, int> kProfileIdc = {
		{"Constrained Baseline", 0x42}, {"Baseline", 0x42},
		{"Main", 0x4D}, {"Extended", 0x58},
		{"High", 0x64}, {"High 10", 0x6E},
		{"High 4:2:2", 0x7A}, {"High 4:4:4 Predictive", 0xF4},
	};
	auto it = kProfileIdc.find(v.profile);
	if (it == kProfileIdc.end() || v.level <= 0) return std::nullopt;
	// ffprobe's "level" is already level_idc's own scale (e.g. 40 for
	// level 4.0) — no conversion needed, just hex-format both bytes.
	char buf[32];
	std::snprintf(buf, sizeof(buf), "avc1.%02x00%02x", it->second, v.level);
	return std::string(buf);
}

static std::optional<std::string> audioCodecString(const AudioTrack& a) {
	if (a.codec == "aac") {
		// mp4a.40.{2,5,29} — LC vs HE-AAC(v1) vs HE-AACv2 object types.
		// ffprobe's AAC "profile" field spells these out directly.
		if (a.profile.find("HE-AACv2") != std::string::npos) return std::string("mp4a.40.29");
		if (a.profile.find("HE-AAC")   != std::string::npos) return std::string("mp4a.40.5");
		return std::string("mp4a.40.2"); // LC — also the safe default for an unrecognized/empty profile
	}
	if (a.codec == "ac3")     return std::string("ac-3");
	if (a.codec == "eac3")    return std::string("ec-3");
	if (a.codec == "mp3")     return std::string("mp3");
	if (a.codec == "flac")    return std::string("fLaC");
	if (a.codec == "opus")    return std::string("Opus");
	if (a.codec == "vorbis")  return std::string("vorbis");
	return std::nullopt;
}

// Builds the #EXT-X-STREAM-INF CODECS attribute value (video codec plus
// every DISTINCT audio codec in use across the AUDIO group — ExoPlayer needs
// all of them declared up front to skip probing each alternate rendition
// itself, not just the currently-active one; see the session's comment
// thread on the audio-track-thrashing bug this fixes). Only for direct-play:
// once direct_play is false either the video or audio stream is actually
// being transcoded, so media_info (the SOURCE file's own probe) no longer
// describes the real output codec/profile — deriving CODECS from it then
// would be exactly the kind of wrong guess this function exists to avoid.
// Returns nullopt (omit CODECS entirely) if any piece can't be confidently
// derived, all-or-nothing — a partial CODECS list covering only some of the
// variant's actual streams is arguably worse than none.
std::optional<std::string> VodSession::buildCodecsAttribute() const {
	if (!direct_play) return std::nullopt;
	std::vector<std::string> parts;
	if (!media_info.video.empty()) {
		auto vcodec = h264CodecString(media_info.video[0]);
		if (!vcodec) return std::nullopt;
		parts.push_back(*vcodec);
	}
	std::set<std::string> distinct_audio;
	for (auto& a : media_info.audio) {
		auto acodec = audioCodecString(a);
		if (!acodec) return std::nullopt;
		distinct_audio.insert(*acodec);
	}
	parts.insert(parts.end(), distinct_audio.begin(), distinct_audio.end());
	if (parts.empty()) return std::nullopt;
	std::ostringstream out;
	for (size_t i = 0; i < parts.size(); ++i) {
		if (i) out << ',';
		out << parts[i];
	}
	return out.str();
}

std::string VodSession::buildMasterPlaylist() const {
	std::ostringstream out;
	out << "#EXTM3U\n#EXT-X-VERSION:6\n";

	// X-PANTHEON-INDEX carries our own track index (Router.cpp's `tracks`
	// JSON / TrackMenu convention) as a custom HLS attribute, verbatim —
	// HLS explicitly allows vendor "X-" attributes on EXT-X-MEDIA, and
	// hls.js exposes ALL attributes generically (MediaPlaylist.attrs, a
	// dictionary, not just the well-known ones). Client code (Hades'
	// VideoPlayer.tsx) reads this back directly instead of matching the URI
	// substring — the URI's own resolution (relative vs. absolute, exact
	// string hls.js records after parsing) isn't something to depend on.
	for (auto& t : media_info.audio) {
		out << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\""
			<< (t.title.empty() ? (t.language.empty() ? "Audio " + std::to_string(t.relative_index) : t.language) : t.title)
			<< "\",LANGUAGE=\"" << t.language << "\",DEFAULT="
			<< (t.relative_index == audio_track_ ? "YES" : "NO")
			<< ",AUTOSELECT=YES,X-PANTHEON-INDEX=\"" << t.relative_index
			<< "\",URI=\"/stream/vod/" << session_id << "/audio/" << t.relative_index << "/playlist.m3u8\"\n";
	}

	// Embedded text tracks (bitmap/burn-in ones have no sidecar — same gate
	// resolveSubtitleTrack()/isTextSubtitleCodec use) plus external sidecars,
	// same index convention as Router.cpp's /stream/vod/start `tracks` JSON.
	// Tracked separately from a raw emptiness check on the source lists
	// below — a file with subtitles that are ALL bitmap-only would otherwise
	// leave GROUP-ID="subs" referenced with no members, invalid per spec.
	bool has_subs = false;
	for (auto& t : media_info.subtitles) {
		if (!isTextSubtitleCodec(t.codec)) continue;
		has_subs = true;
		out << "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\""
			<< (t.title.empty() ? (t.language.empty() ? "Subtitle " + std::to_string(t.relative_index) : t.language) : t.title)
			<< "\",LANGUAGE=\"" << t.language << "\",DEFAULT="
			<< (t.relative_index == subtitle_track_ ? "YES" : "NO")
			<< ",AUTOSELECT=YES,X-PANTHEON-INDEX=\"" << t.relative_index
			<< "\",URI=\"/stream/vod/" << session_id << "/subtitles/" << t.relative_index << "/playlist.m3u8\"\n";
	}
	int ext_index = -2;
	for (auto& t : external_subtitles_) {
		has_subs = true;
		out << "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\""
			<< (t.language.empty() ? "Subtitle " + std::to_string(ext_index) : t.language)
			<< "\",LANGUAGE=\"" << t.language << "\",DEFAULT="
			<< (ext_index == subtitle_track_ ? "YES" : "NO")
			<< ",AUTOSELECT=YES,X-PANTHEON-INDEX=\"" << ext_index
			<< "\",URI=\"/stream/vod/" << session_id << "/subtitles/" << ext_index << "/playlist.m3u8\"\n";
		--ext_index;
	}

	// Single variant — same combined-AV playlist.m3u8 already served today,
	// now also referencing the AUDIO/SUBTITLES groups above so native
	// players know they exist. CODECS is included when buildCodecsAttribute
	// can derive it confidently (direct-play + recognized codecs) — see its
	// own comment. Without it, ExoPlayer in particular has no static way to
	// know each AUDIO-group rendition's format and ends up opening/probing
	// every one, which (since ensureAudioTrack treats any probe hitting a
	// non-active track exactly like a real switch) was triggering a real
	// encoder restart per alternate audio language on session start.
	auto codecs = buildCodecsAttribute();
	out << "#EXT-X-STREAM-INF:BANDWIDTH=" << estimateBandwidthBps(media_info)
		<< (media_info.audio.empty() ? "" : ",AUDIO=\"audio\"")
		<< (has_subs ? ",SUBTITLES=\"subs\"" : "")
		<< (codecs ? ",CODECS=\"" + *codecs + "\"" : "")
		<< "\n/stream/vod/" << session_id << "/playlist.m3u8\n";
	return out.str();
}

std::optional<std::string> VodSession::buildSubtitlePlaylist(int track) const {
	auto resolved = resolveSubtitleTrack(track);
	std::cerr << "[vod:" << session_id << "] buildSubtitlePlaylist track=" << track
			  << " output=" << (resolved.output ? "yes" : "no")
			  << " burn_in=" << (resolved.burn_in ? "yes" : "no")
			  << " external_path=" << (resolved.external_path.empty() ? "(embedded)" : resolved.external_path) << "\n";
	if (!resolved.output) return std::nullopt;
	int64_t duration_ms = effective_duration_ms > 0 ? effective_duration_ms : 1000;
	int target_duration_secs = static_cast<int>(std::ceil(duration_ms / 1000.0));
	std::ostringstream out;
	out << std::fixed << std::setprecision(3);
	out << "#EXTM3U\n"
		<< "#EXT-X-VERSION:3\n"
		<< "#EXT-X-TARGETDURATION:" << target_duration_secs << "\n"
		<< "#EXT-X-PLAYLIST-TYPE:VOD\n"
		<< "#EXTINF:" << (duration_ms / 1000.0) << ",\n"
		<< "/stream/vod/" << session_id << "/subtitles/" << track << "\n"
		<< "#EXT-X-ENDLIST\n";
	return out.str();
}

bool VodSession::restartAt(int segment_index) {
	if (total_segments <= 0) return false; // start() always sets this before the first call
	segment_index = std::clamp(segment_index, 0, total_segments - 1);
	// segment_start_ms[segment_index], not segment_index * kVodHlsSegmentSecs
	// — for direct-play these are the file's real (non-uniform) keyframe cut
	// points, not a synthetic uniform cadence (see the class comment on
	// segment_start_ms).
	int64_t position_ms = segment_start_ms[static_cast<size_t>(segment_index)];

	const VideoTrack* source_video = media_info.video.empty() ? nullptr : &media_info.video[0];
	auto args = buildVodArgs(ffmpeg_path, file_path,
							  position_ms,
							  audio_track_, subtitle_track_, direct_play, subtitle_burn_in,
							  opts.hw_accel, opts.vaapi_device, opts.decode_hw_accel,
							  opts.decodable_codecs, source_codec_, source_video,
							  hdr_capable_, opts.verbose_transcode_logs, dir(),
							  segment_index);

	std::lock_guard<std::mutex> lock(session_mtx);
	if (!active.load()) return false;

	bool first_run = !has_spawned_once_;
	if (ffmpeg) { ffmpeg->kill(); ffmpeg.reset(); } // now SIGCONT+SIGTERM-safe against a paused encoder

	std::cerr << "[vod:" << session_id << "] " << (first_run ? "spawning" : "restarting")
			  << " main encoder at segment " << segment_index << " (offset="
			  << (position_ms / 1000.0) << "s)\n";

	ffmpeg = std::make_unique<FfmpegProcess>(
		std::move(args),
		/*on_data=*/nullptr, // output goes to disk, not stdout — nothing to fan out
		[this](int code) { onMainEncoderExit(code); },
		opts.buffer_size,
		opts.ffmpeg_debug_logs,
		opts.verbose_transcode_logs
	);
	if (!ffmpeg->start()) {
		std::cerr << "[vod:" << session_id << "] failed to spawn main encoder\n";
		ffmpeg.reset();
		return false;
	}

	has_spawned_once_ = true;
	current_run_exited_naturally.store(false);
	current_run_start_segment.store(segment_index);
	highest_generated_segment.store(segment_index - 1);
	if (!first_run) discontinuity_boundaries.push_back(segment_index);
	writePlaylist();
	return true;
}

void VodSession::spawnSubtitleProcess() {
	if (!subtitle_output) return;
	std::lock_guard<std::mutex> lock(subs_mtx);
	if (subs_ffmpeg) return; // already spawned — idempotent, callable from the lazy /subs.vtt route
	auto args = buildVodSubtitleArgs(ffmpeg_path, file_path, subtitle_track_,
									  external_subtitle_path_, opts.verbose_transcode_logs,
									  dir() + "/subs.vtt");
	subs_ffmpeg = std::make_unique<FfmpegProcess>(
		std::move(args), nullptr,
		[this](int code) { if (code != 0) subs_failed.store(true); subs_exited.store(true); },
		opts.buffer_size, opts.ffmpeg_debug_logs, opts.verbose_transcode_logs
	);
	// Best-effort — hasSubtitleOutput()/subtitle_url still get returned even
	// if this fails to spawn; Router's wait just times out and 503s, same
	// failure mode as today.
	if (!subs_ffmpeg->start()) {
		std::cerr << "[vod:" << session_id << "] failed to spawn subtitle extraction\n";
		subs_ffmpeg.reset();
	}
}

std::unique_ptr<FfmpegProcess> VodSession::spawnSubtitlePipe(int track, DataCallback on_data, ExitCallback on_exit) const {
	auto resolved = resolveSubtitleTrack(track);
	std::cerr << "[vod:" << session_id << "] spawnSubtitlePipe requested track=" << track
			  << " resolved.output=" << (resolved.output ? "yes" : "no")
			  << " resolved.external_path=" << (resolved.external_path.empty() ? "(embedded)" : resolved.external_path) << "\n";
	if (!resolved.output) {
		std::cerr << "[vod:" << session_id << "] spawnSubtitlePipe track=" << track << " has no text output — refusing\n";
		return nullptr;
	}
	// buildVodSubtitleArgs ignores subtitleTrack when externalSubtitlePath is
	// set (the sidecar file becomes the sole -i, always mapped as 0:s:0) —
	// track only means something for the embedded branch.
	auto args = buildVodSubtitleArgs(ffmpeg_path, file_path, track,
									  resolved.external_path, opts.verbose_transcode_logs,
									  /*outPath=*/"", /*on_fly=*/true);
	auto proc = std::make_unique<FfmpegProcess>(
		std::move(args), std::move(on_data), std::move(on_exit),
		opts.buffer_size, opts.ffmpeg_debug_logs, opts.verbose_transcode_logs
	);
	if (!proc->start()) {
		std::cerr << "[vod:" << session_id << "] failed to spawn on-demand subtitle pipe for track " << track << "\n";
		return nullptr;
	}
	std::cerr << "[vod:" << session_id << "] spawned on-demand subtitle pipe ffmpeg for track " << track << "\n";
	return proc;
}

std::string VodSession::segmentPath(int index) const {
	char buf[24];
	std::snprintf(buf, sizeof(buf), "seg-%05d.ts", index);
	return dir() + "/" + buf;
}

// Caller must hold session_mtx (only ever called from restartAt()).
std::string VodSession::buildStaticPlaylist() const {
	// #EXT-X-TARGETDURATION must be an upper bound (rounded up) on every
	// segment's actual duration per spec — direct-play segments can run
	// longer than kVodHlsSegmentSecs (cut at real keyframes, not a forced
	// cadence), so this can't just be the nominal constant anymore.
	int64_t max_seg_ms = int64_t(kVodHlsSegmentSecs) * 1000;
	for (int i = 0; i < total_segments; ++i) {
		int64_t seg_end = (i + 1 < total_segments) ? segment_start_ms[static_cast<size_t>(i + 1)] : effective_duration_ms;
		max_seg_ms = std::max(max_seg_ms, seg_end - segment_start_ms[static_cast<size_t>(i)]);
	}
	int target_duration_secs = static_cast<int>(std::ceil(max_seg_ms / 1000.0));

	std::ostringstream out;
	out << "#EXTM3U\n"
		<< "#EXT-X-VERSION:3\n"
		<< "#EXT-X-TARGETDURATION:" << target_duration_secs << "\n"
		<< "#EXT-X-PLAYLIST-TYPE:VOD\n";

	for (int i = 0; i < total_segments; ++i) {
		if (std::find(discontinuity_boundaries.begin(), discontinuity_boundaries.end(), i)
				!= discontinuity_boundaries.end())
			out << "#EXT-X-DISCONTINUITY\n";

		int64_t seg_start = segment_start_ms[static_cast<size_t>(i)];
		int64_t seg_end = (i + 1 < total_segments) ? segment_start_ms[static_cast<size_t>(i + 1)] : effective_duration_ms;
		double dur_secs = std::max(0.1, (seg_end - seg_start) / 1000.0);

		char extinf[32];
		std::snprintf(extinf, sizeof(extinf), "%.3f", dur_secs);
		out << "#EXTINF:" << extinf << ",\n";

		char seg[24];
		std::snprintf(seg, sizeof(seg), "seg-%05d.ts", i);
		out << seg << "\n";
	}

	out << "#EXT-X-ENDLIST\n";
	return out.str();
}

// Caller must hold session_mtx (only ever called from restartAt()).
void VodSession::writePlaylist() const {
	std::ofstream f(dir() + "/playlist.m3u8", std::ios::trunc);
	f << buildStaticPlaylist();
}

VodSession::SegmentPrep VodSession::prepareSegment(int segment_index) {
	if (!active.load()) return SegmentPrep::Failed;
	if (segment_index < 0 || segment_index >= total_segments) return SegmentPrep::Failed;

	last_requested_segment.store(segment_index);
	// Reached the real start at least once — a hole before it now is a real seek.
	// if (segment_index >= initial_start_segment_) initial_target_reached.store(true);

	int run_start, generated;
	bool run_exhausted;
	{
		std::lock_guard<std::mutex> lock(session_mtx);
		run_start = current_run_start_segment.load();
		generated = highest_generated_segment.load();
		run_exhausted = current_run_exited_naturally.load();
	}

	std::cerr << "[vod:" << session_id << "] prepareSegment seg=" << segment_index
			  << " run_start=" << run_start << " generated=" << generated
			  << " run_exhausted=" << (run_exhausted ? "yes" : "no") << "\n";

	if (segment_index >= run_start && segment_index <= generated) {
		// Already on disk. Resume unconditionally on any request, not just
		// near the frontier — cutting it close caused visible stutter.
		// maybeAutoPause() re-pauses once genuinely idle again.
		std::lock_guard<std::mutex> lock(session_mtx);
		if (ffmpeg && ffmpeg->isPaused()) ffmpeg->resume();
		return SegmentPrep::Ready;
	}

	// highest_generated_segment only advances on the lookahead monitor's
	// ~2s tick (scanGeneratedProgress()), so a segment that just finished
	// writing — or, for a very fast direct-play remux, several at once —
	// can briefly look "not generated" here even though the file already
	// exists on disk. A cheap direct existence check avoids mistaking that
	// staleness for "this run is exhausted and will never produce it" and
	// triggering a wasted restart below.
	if (segment_index > generated && std::filesystem::exists(segmentPath(segment_index))) {
		std::cerr << "[vod:" << session_id << "] prepareSegment seg=" << segment_index
				  << " on disk despite stale highest_generated_segment — treating as Ready\n";
		std::lock_guard<std::mutex> lock(session_mtx);
		if (segment_index > highest_generated_segment.load()) highest_generated_segment.store(segment_index);
		if (ffmpeg && ffmpeg->isPaused()) ffmpeg->resume();
		return SegmentPrep::Ready;
	}

	// Needed segment isn't generated yet. If this run's encoder already
	// finished on its own without ever producing it (e.g. a direct-play
	// remux whose real keyframe cadence made it exit before this index was
	// reached — shouldn't happen now that segment_start_ms mirrors ffmpeg's
	// own real cut points, but a crash mid-encode has the same shape),
	// waiting is pointless: that process is never coming back for it.
	// Restart there directly instead of burning a wait budget on a run
	// that's already exhausted.
	if (segment_index > generated && !run_exhausted && segment_index <= generated + kVodCatchUpMarginSegments) {
		std::cerr << "[vod:" << session_id << "] prepareSegment seg=" << segment_index
				  << " within catch-up margin of generated=" << generated << " — WaitShort\n";
		std::lock_guard<std::mutex> lock(session_mtx);
		if (ffmpeg && ffmpeg->isPaused()) ffmpeg->resume();
		return SegmentPrep::WaitShort;
	}

	// A hole before this run's start is usually a genuine backward seek —
	// EXCEPT before the session has ever reached its real starting point
	// (fresh resume/track-switch): some clients probe an early segment
	// before honoring the real start position, and blindly restarting on
	// that would reset the whole session to 0:00. Once the real target has
	// been reached once (initial_target_reached), a hole really is a seek.
	//if (segment_index < run_start && !initial_target_reached.load()) {
	//	return SegmentPrep::WaitShort; // times out to 503 if it never appears — never restarts here
	//}

	// Forward jump far beyond reach, or a genuine backward seek past this
	// run's start — restarting there directly is cheaper than waiting for
	// the encoder to churn there sequentially (or, for the backward case, it
	// never would on its own).
	std::cerr << "[vod:" << session_id << "] prepareSegment seg=" << segment_index
			  << " outside generated range and catch-up margin — restarting (WaitColdStart)\n";
	if (!restartAt(segment_index)) return SegmentPrep::Failed;
	return SegmentPrep::WaitColdStart;
}

void VodSession::scanGeneratedProgress() {
	std::lock_guard<std::mutex> lock(session_mtx);
	int idx = highest_generated_segment.load() + 1;
	while (idx < total_segments && std::filesystem::exists(segmentPath(idx))) {
		highest_generated_segment.store(idx);
		++idx;
	}
}

void VodSession::maybeAutoPause() {
	std::lock_guard<std::mutex> lock(session_mtx);
	if (!ffmpeg) return;
	int generated = highest_generated_segment.load();
	if (generated >= total_segments - 1) return; // fully generated — nothing left to pace

	int floor = std::max(last_requested_segment.load(), current_run_start_segment.load());
	int window_segments = std::max(1, opts.lookahead_secs / kVodHlsSegmentSecs);
	// Hysteresis: pause at the full window, resume at half — gives the
	// encoder real runway before the client could catch up to it.
	int resume_threshold_segments = std::max(1, window_segments / 2);
	bool should_pause  = (generated - floor) >= window_segments;
	bool should_resume = (generated - floor) < resume_threshold_segments;

	if (should_pause && !ffmpeg->isPaused()) ffmpeg->pause();
	else if (should_resume && ffmpeg->isPaused()) ffmpeg->resume();
}

void VodSession::lookaheadLoop() {
	while (!lookahead_stop.load() && active.load()) {
		std::this_thread::sleep_for(std::chrono::seconds(kLookaheadTickSecs));
		if (!active.load() || lookahead_stop.load()) return;
		scanGeneratedProgress();
		maybeAutoPause();
	}
}

bool VodSession::isMainEncoderPaused() const {
	// Not locking session_mtx here — this is a best-effort debug/activity-
	// view accessor (see ActivityRouter), not something correctness depends
	// on; a torn read just means one stale poll of the panel, never a
	// playback issue.
	return ffmpeg && ffmpeg->isPaused();
}

void VodSession::onMainEncoderExit(int code) {
	std::cerr << "[vod:" << session_id << "] main encoder exited (code=" << code << ")\n";
	// Natural completion (reached real EOF) or a crash — either way this run
	// isn't producing anything further. VOD has no "next item" to transition
	// to the way a channel does; prepareSegment() uses this flag to notice a
	// still-missing segment this run will never produce (see its own
	// comment) rather than waiting out a doomed budget.
	current_run_exited_naturally.store(true);
}

void VodSession::stop() {
	if (!active.exchange(false)) return;

	lookahead_stop = true;
	if (lookahead_thread.joinable()) lookahead_thread.join();

	{
		std::lock_guard<std::mutex> lock(session_mtx);
		if (ffmpeg) { ffmpeg->kill(); ffmpeg.reset(); }
	}
	{
		std::lock_guard<std::mutex> lock(subs_mtx);
		if (subs_ffmpeg) { subs_ffmpeg->kill(); subs_ffmpeg.reset(); }
	}

	std::error_code ec;
	std::filesystem::remove_all(dir(), ec);
}

void VodSession::touch() { last_touch_ms.store(nowMs()); }

bool VodSession::isIdle() const {
	int64_t touch = last_touch_ms.load();
	if (touch == 0) return false; // never touched yet — still starting up, don't reap
	return (nowMs() - touch) > static_cast<int64_t>(opts.linger_secs) * 1000;
}
