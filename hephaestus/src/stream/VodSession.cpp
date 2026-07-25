#include "VodSession.h"
#include "EncoderArgs.h"
#include "VodSessionManager.h"
#include "thread/TaskRegistry.h"
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

// Same last_write_time-to-epoch conversion Kairos's own sync-time probe uses
// to fingerprint a file (SyncManager.cpp's statFingerprint) — kept as a
// separate copy for the same reason the rest of this file's MediaProbe is:
// different service/binary, not shared code. 0 on any stat error, which a
// real file's mtime will practically never equal, so a stat failure here
// just always misses the cache rather than risking a false "unchanged."
static int64_t statMtimeEpochSecs(const std::string& path)
{
	std::error_code ec;
	auto ftime = std::filesystem::last_write_time(path, ec);
	if (ec) return 0;
	auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
	return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count());
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

// Replicates ffmpeg's own -hls_time cutting rule for a direct-stream (stream
// copy) session: cut at the first keyframe at or after hls_time_secs seconds
// have elapsed since the last cut. Given the file's real keyframe timestamps
// (MediaProbe::probeKeyframeTimestampsMs), this predicts EXACTLY where a
// -c:v copy invocation will actually cut segments, since stream copy can't
// force keyframes onto any other cadence — the assumed-uniform-cadence
// approach only holds for transcode/burn-in, where -force_key_frames
// controls placement directly. Returns an empty vector if keyframes_ms is
// empty (probe failed) — callers fall back to the uniform assumption.
static std::vector<int64_t> simulateDirectStreamSegmentBoundaries(
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
// stream-copy for direct-stream). A resolution-based ladder guess plus the
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
// capability (see ClientCapabilities.h). When it has, direct-stream is
// decided against that declared set instead — native Android via
// MediaCodec, or a real TV's hardware decoder, can often handle far more
// than a browser's <video>/hls.js path can (hevc, av1, ac3, ...), and a
// fixed global allowlist has no way to take advantage of that.
//
// Split into independent video/audio checks now that video and audio are
// independent streams (VodEncodeStream) — previously a single combined
// isDirectStreamable(info, audioTrack, caps) meant an incompatible AUDIO
// codec forced a VIDEO re-encode too, even when the video itself was
// perfectly direct-streamable. That coupling is gone: each stream decides its
// own eligibility from its own codec alone.
static bool isVideoDirectStreamable(const MediaInfo& info, const std::optional<ClientCapabilities>& client_caps)
{
	if (info.video.empty()) return false;
	const std::string& videoCodec = info.video[0].codec;
	if (client_caps && client_caps->max_height && info.video[0].height > *client_caps->max_height) return false; // can't downscale a stream copy
	if (client_caps) return client_caps->video_codecs.count(videoCodec) > 0;
	return videoCodec == "h264";
}

static bool isAudioDirectStreamable(const MediaInfo& info, int audioTrack,
									const std::optional<ClientCapabilities>& client_caps) {
	auto it = std::find_if(info.audio.begin(), info.audio.end(),
		[&](const AudioTrack& t) { return t.relative_index == audioTrack; });
	if (it == info.audio.end()) return false;
	const std::string& audioCodec = it->codec;
	if (client_caps) return client_caps->audio_codecs.count(audioCodec) > 0;
	return audioCodec == "aac";
}

// On-disk homes for shared VodEncodeStream segments — keyed by content + the
// resolved transcode decision, not any single viewer's session_id, since
// multiple VodSession facades can now reference the same stream (see
// VodSessionManager.h's class comment). Lives under hls_root/vod-streams/
// rather than hls_root/vod/<session_id>/ (a real per-viewer session
// directory, still used for each viewer's own manifest/subs.vtt) to keep the
// two namespaces visually distinct on disk.
static std::string videoStreamDir(const std::string& hls_root, const VideoStreamKey& key) {
	std::ostringstream ss;
	ss << hls_root << "/vod-streams/" << key.content_id << "-v-"
	   << (key.direct_stream ? "direct" : key.video_codec)
	   << (key.hdr_capable ? "-hdr" : "-sdr")
	   << "-burn" << key.burn_in_track
	   << "-h" << key.max_height;
	return ss.str();
}

static std::string audioStreamDir(const std::string& hls_root, const AudioStreamKey& key) {
	std::ostringstream ss;
	ss << hls_root << "/vod-streams/" << key.content_id << "-a" << key.audio_track << "-"
	   << (key.direct_stream ? "direct" : key.audio_codec);
	return ss.str();
}

// The video codec a resolved (non-direct-stream) transcode will actually use —
// mirrors pushVideoEncoderArgs' own branching (EncoderArgs.cpp) exactly, so
// the VideoStreamKey this produces always matches the real encode a factory
// lambda built from the same inputs spawns. HDR passthrough is always HEVC
// Main10 regardless of source codec/client_caps — see pushVideoEncoderArgs'
// own comment — so this is only ever consulted for the non-passthrough path;
// callers pass source_hdr/hdr_capable purely to decide which branch, not
// into this function itself.
static std::string resolveVideoCodecForKey(const std::string& source_codec,
											const std::optional<ClientCapabilities>& client_caps) {
	return chooseVideoCodec(source_codec, client_caps).name;
}

// Both video and audio args need this in exactly the same spot (right
// before the output path) — output-side duration bound (bounds a head to
// its own window) plus the absolute-timeline timestamp rebasing that
// replaces #EXT-X-DISCONTINUITY (see the class comment thread this came out
// of): without it, each head's ffmpeg process resets its own PTS clock near
// zero on every restart, so segment N from one head and segment N+1 from a
// different one would carry genuinely discontinuous timestamps even though
// the static playlist declares them as one continuous file. -output_ts_offset
// rebases this run's output onto the real absolute position instead, so
// every head's segments land in the same timestamp domain regardless of
// which one produced them — the same mechanism Kyoo's own transcoder uses
// for exactly this.
static void pushHeadBoundArgs(std::vector<std::string>& a, int64_t positionMs, std::optional<double> windowDurationSecs) {
	std::ostringstream offset;
	offset << std::fixed << std::setprecision(3) << (positionMs / 1000.0);
	a.insert(a.end(), {"-output_ts_offset", offset.str()});
	if (windowDurationSecs) {
		std::ostringstream dur;
		dur << std::fixed << std::setprecision(3) << *windowDurationSecs;
		a.insert(a.end(), {"-t", dur.str()});
	}
}

// Video-only now — see isVideoDirectStreamable()'s own comment on why this no
// longer takes an audio track at all. hlsStartNumber is the segment index
// this head starts at (its own window, not necessarily 0) — every head goes
// through VodEncodeStream::spawnHead(), which is what guarantees output
// numbering always matches the static playlist's declared segment list.
static std::vector<std::string> buildVodVideoArgs(
	const std::string& ffmpeg_path,
	const std::string& file_path,
	int64_t positionMs,
	std::optional<double> windowDurationSecs,
	int subtitleTrack,
	bool directStream,
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
	int hlsStartNumber,
	const std::optional<ClientCapabilities>& client_caps)
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

	// Direct-stream is a pure stream copy — nothing gets decoded, so a decode
	// hwaccel would be a pointless no-op at best.
	if (!directStream) pushHwAccelDecodeArgs(a, decode_hw_accel, decodable_codecs, source_codec);

	a.push_back("-i"); a.push_back(file_path);

	if (directStream)
	{
		a.insert(a.end(), {"-map", "0:v:0?", "-dn", "-map_chapters", "-1", "-c:v", "copy"});
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
		pushVideoEncoderArgs(a, vfParts, hw_accel, kVodHlsSegmentSecs, source_video, hdr_capable, client_caps);
		std::string filterComplex = "[0:v:0][0:s:" + std::to_string(subtitleTrack) + "]overlay";
		for (auto& p : vfParts) filterComplex += "," + p;
		filterComplex += "[vout]";
		a.insert(a.end(), {"-filter_complex", filterComplex});
		a.insert(a.end(), {"-map", "[vout]", "-dn", "-map_chapters", "-1"});
	} else {
		a.insert(a.end(), {"-map", "0:v:0?", "-dn", "-map_chapters", "-1"});
		std::vector<std::string> vfParts;
		pushVideoEncoderArgs(a, vfParts, hw_accel, kVodHlsSegmentSecs, source_video, hdr_capable, client_caps);
		pushVideoFilterArgs(a, vfParts);
	}

	pushHeadBoundArgs(a, positionMs, windowDurationSecs);

	// "vod" playlist type (not "event" as this used to be): ffmpeg's own
	// playlist output here is now purely a private scratch file Hephaestus
	// itself never serves — VodSession synthesizes and serves the real,
	// complete, #EXT-X-ENDLIST-terminated playlist.m3u8 directly (see
	// buildStaticPlaylist()), so the old "does ffmpeg's own HLS muxer emit
	// the playlist incrementally or hold it back" distinction that used to
	// matter here no longer applies to anything a client ever sees.
	a.insert(a.end(), {
		"-f", "hls",
		"-hls_time", std::to_string(kVodHlsSegmentSecs),
		"-hls_playlist_type", "vod",
		"-hls_list_size", "0",
		"-start_number", std::to_string(hlsStartNumber),
		"-hls_segment_filename", dir + "/seg-%05d.ts",
		dir + "/video-encoder.m3u8"
	});

	return a;
}

// Independent audio elementary stream — its own HLS segment timeline in the
// same session directory (distinct "aseg-" filename prefix so it never
// collides with the video stream's "seg-" segments), no video mapped at
// all. directStream here is this ONE track's own eligibility (isAudioDirectStreamable),
// entirely independent of whatever the video stream decided for itself —
// see isVideoDirectStreamable/isAudioDirectStreamable's shared comment.
static std::vector<std::string> buildVodAudioArgs(
	const std::string& ffmpeg_path,
	const std::string& file_path,
	int64_t positionMs,
	std::optional<double> windowDurationSecs,
	int audioTrack,
	bool directStream,
	bool verbose_transcode_logs,
	const std::string& dir,
	int hlsStartNumber,
	const std::optional<ClientCapabilities>& client_caps,
	const AudioTrack* source_audio)
{
	std::vector<std::string> a;
	a.push_back(ffmpeg_path);
	pushLogLevelArgs(a, verbose_transcode_logs);

	a.insert(a.end(), {"-fflags", "+genpts+discardcorrupt"});

	if (positionMs > 0) {
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(3) << (positionMs / 1000.0);
		a.push_back("-ss"); a.push_back(ss.str());
	}

	a.push_back("-i"); a.push_back(file_path);

	a.insert(a.end(), {"-map", "0:a:" + std::to_string(audioTrack) + "?", "-dn", "-map_chapters", "-1"});
	if (directStream) a.insert(a.end(), {"-c:a", "copy"});
	else            pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, /*audio_bitrate_kbps=*/192, client_caps, source_audio);

	pushHeadBoundArgs(a, positionMs, windowDurationSecs);

	a.insert(a.end(), {
		"-f", "hls",
		"-hls_time", std::to_string(kVodHlsSegmentSecs),
		"-hls_playlist_type", "vod",
		"-hls_list_size", "0",
		"-start_number", std::to_string(hlsStartNumber),
		"-hls_segment_filename", dir + "/aseg-%05d.ts",
		dir + "/audio-encoder.m3u8"
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

VodSession::VodSession(std::string session_id, std::string content_type, std::string content_id,
						std::string ffmpeg_path, VodStreamOptions opts, VodSessionManager& manager)
	: session_id(std::move(session_id)), content_type_(std::move(content_type)), content_id_(std::move(content_id)),
	  ffmpeg_path(std::move(ffmpeg_path)), opts(std::move(opts)), manager_(manager) {}

VodSession::~VodSession() { stop(); }

bool VodSession::start(const std::string& file_path, int64_t position_ms,
						int audio_track, int subtitle_track, bool hdr_capable,
						const std::optional<ClientCapabilities>& client_caps,
						const std::vector<ExternalSubtitle>& external_subtitles,
						int64_t fallback_duration_ms,
						const std::string& preferred_audio_lang,
						const std::string& preferred_subtitle_lang,
						const std::vector<int64_t>& kairos_keyframes_ms,
						int64_t kairos_keyframes_size,
						int64_t kairos_keyframes_mtime)
{
	external_subtitles_     = external_subtitles;
	kairos_keyframes_ms_    = kairos_keyframes_ms;
	kairos_keyframes_size_  = kairos_keyframes_size;
	kairos_keyframes_mtime_ = kairos_keyframes_mtime;
	auto info               = probeMediaCached(opts.ffprobe_path, file_path);
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
	direct_stream   = isVideoDirectStreamable(media_info, client_caps);

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
		std::cerr << " -> direct_stream=" << (direct_stream ? "yes" : "no") << "\n";
	}

	auto resolved = resolveSubtitleTrack(subtitle_track);
	subtitle_output          = resolved.output;
	subtitle_burn_in         = resolved.burn_in;
	external_subtitle_path_  = resolved.external_path;
	// Burning a subtitle onto the video means decoding and re-encoding it —
	// direct stream (a pure stream copy) is incompatible with that.
	if (subtitle_burn_in) direct_stream = false;

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
	// Both playlists are a pure function of segment_start_ms/total_segments —
	// fixed for the session's whole life now that neither video nor audio
	// ever recomputes them after this point (an audio track switch no longer
	// touches video's own boundaries — see ensureAudioTrack()) — so this is
	// the only place either ever needs writing, not on every segment request
	// the way the old per-restart discontinuity-accumulating playlist needed.
	{
		std::lock_guard<std::mutex> lock(session_mtx);
		writeVideoPlaylist();
		writeAudioPlaylist();
	}

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

	// Resolved decision, not raw inputs — see VideoStreamKey's own comment
	// (VodSessionManager.h) on why: two viewers whose raw hdr_capable/
	// max_height differ but land on the same actual ffmpeg invocation must
	// still share, and direct-stream's key fields are forced to their neutral
	// values below since a stream copy is identical regardless of what a
	// client declared (nothing about it is actually decided by those flags).
	bool source_hdr               = !media_info.video.empty() && isHdrTransfer(media_info.video[0].color_transfer);
	bool resolved_hdr_passthrough = !direct_stream && source_hdr && hdr_capable_;
	int resolved_max_height       = direct_stream ? 0
								  : (client_caps_ && client_caps_->max_height ? *client_caps_->max_height : 0);
	std::string resolved_video_codec = direct_stream ? std::string()
										   : (resolved_hdr_passthrough ? "hevc" : resolveVideoCodecForKey(source_codec_, client_caps_));

	VideoStreamKey vkey{
		content_id_, direct_stream, resolved_hdr_passthrough,
		subtitle_burn_in ? subtitle_track_ : -1,
		resolved_video_codec, resolved_max_height
	};
	std::string vdir = videoStreamDir(opts.hls_root, vkey);

	// Captured by VALUE, not `this` — the factory/ArgsBuilder closures live
	// inside the shared VodEncodeStream, which can outlive THIS VodSession
	// (a different viewer's facade may still be referencing it after this
	// session stops). Every input a later head-spawn needs must be a
	// self-contained copy, not a pointer back into a session that might
	// already be gone.
	std::string ffmpeg_path_copy = ffmpeg_path;
	std::string file_path_copy   = this->file_path;
	VodStreamOptions opts_copy   = opts;
	std::optional<ClientCapabilities> client_caps_copy = client_caps_;
	std::string source_codec_copy = source_codec_;
	std::optional<VideoTrack> source_video_copy;
	if (!media_info.video.empty()) source_video_copy = media_info.video[0];
	bool hdr_capable_copy = hdr_capable_;
	int burn_in_track     = vkey.burn_in_track;
	bool video_direct     = direct_stream;
	bool video_burn_in    = subtitle_burn_in;

	video_stream_ = manager_.getOrCreateVideoStream(vkey,
		[vdir, ffmpeg_path_copy, file_path_copy, opts_copy, client_caps_copy, source_codec_copy,
		 source_video_copy, hdr_capable_copy, burn_in_track, video_direct, video_burn_in]() {
			std::error_code ec;
			std::filesystem::create_directories(vdir, ec);
			return std::make_shared<VodEncodeStream>(
				"video", vdir, "seg-",
				[ffmpeg_path_copy, file_path_copy, opts_copy, client_caps_copy, source_codec_copy,
				 source_video_copy, hdr_capable_copy, burn_in_track, video_direct, video_burn_in, vdir]
				(int segment_index, int64_t posMs, std::optional<double> windowSecs) {
					const VideoTrack* source_video = source_video_copy ? &*source_video_copy : nullptr;
					return buildVodVideoArgs(ffmpeg_path_copy, file_path_copy, posMs, windowSecs,
											  burn_in_track, video_direct, video_burn_in,
											  opts_copy.hw_accel, opts_copy.vaapi_device, opts_copy.decode_hw_accel,
											  opts_copy.decodable_codecs, source_codec_copy, source_video,
											  hdr_capable_copy, opts_copy.verbose_transcode_logs, vdir, segment_index,
											  client_caps_copy);
				},
				opts_copy.buffer_size, opts_copy.ffmpeg_debug_logs, opts_copy.verbose_transcode_logs,
				opts_copy.lookahead_secs, kVodHlsSegmentSecs);
		});
	if (!video_stream_ ||
		video_stream_->prepareSegment(start_segment, segment_start_ms, total_segments) == VodEncodeStream::SegmentPrep::Failed) {
		std::cerr << "[vod:" << session_id << "] failed to spawn initial video head\n";
		active = false;
		return false;
	}

	rebuildAudioStream(start_segment);

	lookahead_stop = false;
	lookahead_thread = std::thread([this] { lookaheadLoop(); });

	std::cerr << "[vod:" << session_id << "] started: \"" << file_path << "\""
			  << " duration=" << effective_duration_ms << "ms segments=" << total_segments
			  << " direct_stream=" << (direct_stream ? "yes" : "no") << "\n";
	return true;
}

// Builds a fresh audio_stream_ targeting audio_track_ and primes its first
// head at target_segment. Called from start() (target_segment = the
// session's initial position) and ensureAudioTrack() (target_segment =
// wherever the viewer currently is) — either way this is the only place
// that constructs an audio VodEncodeStream, so its ArgsBuilder's captured
// track/direct-stream decision can never drift from audio_track_.
void VodSession::rebuildAudioStream(int target_segment) {
	if (media_info.audio.empty()) { audio_stream_.reset();
		return;
	}
	int captured_track = audio_track_;
	bool audio_direct  = isAudioDirectStreamable(media_info, captured_track, client_caps_);
	auto it            = std::find_if(media_info.audio.begin(), media_info.audio.end(),
		[&](const AudioTrack& t) { return t.relative_index == captured_track; });
	std::optional<AudioTrack> source_audio_copy;
	if (it != media_info.audio.end()) source_audio_copy = *it;

	AudioStreamKey akey{
		content_id_, captured_track, audio_direct,
		audio_direct ? std::string() : chooseAudioCodec(it != media_info.audio.end() ? &*it : nullptr, client_caps_).name
	};
	std::string adir = audioStreamDir(opts.hls_root, akey);

	// Same by-value-capture reasoning as the video stream above — this
	// closure lives inside the shared VodEncodeStream, not this session.
	std::string ffmpeg_path_copy = ffmpeg_path;
	std::string file_path_copy   = this->file_path;
	VodStreamOptions opts_copy   = opts;
	std::optional<ClientCapabilities> client_caps_copy = client_caps_;

	audio_stream_ = manager_.getOrCreateAudioStream(akey,
		[adir, ffmpeg_path_copy, file_path_copy, opts_copy, client_caps_copy, captured_track, audio_direct,
		 source_audio_copy]() {
			std::error_code ec;
			std::filesystem::create_directories(adir, ec);
			return std::make_shared<VodEncodeStream>(
				"audio", adir, "aseg-",
				[ffmpeg_path_copy, file_path_copy, opts_copy, client_caps_copy, captured_track, audio_direct,
				 source_audio_copy, adir](int segment_index, int64_t posMs, std::optional<double> windowSecs) {
					const AudioTrack* source_audio = source_audio_copy ? &*source_audio_copy : nullptr;
					return buildVodAudioArgs(ffmpeg_path_copy, file_path_copy, posMs, windowSecs, captured_track,
											  audio_direct, opts_copy.verbose_transcode_logs, adir, segment_index,
											  client_caps_copy, source_audio);
				},
				opts_copy.buffer_size, opts_copy.ffmpeg_debug_logs, opts_copy.verbose_transcode_logs,
				opts_copy.lookahead_secs, kVodHlsSegmentSecs);
		});
	audio_stream_->prepareSegment(target_segment, segment_start_ms, total_segments);
}

void VodSession::computeSegmentBoundaries() {
	segment_start_ms.clear();
	if (direct_stream) {
		// Stream copy can only cut where a real keyframe already exists —
		// probe them and predict ffmpeg's own exact cut points rather than
		// assuming a uniform cadence the encoder has no way to honor. Kairos
		// caches this exact scan from its own sync-time probe (see
		// Database.cpp's v98 migration) — trust it only if the file's
		// current stat() still matches what it was computed from (a stable
		// path never proves a stable file: Sonarr/Radarr-style upgrades
		// replace a library file in place). Falls back to a live probe here
		// otherwise, same as always, and pushes the fresh result back to
		// Kairos so the next session on this file doesn't pay for it again.
		std::vector<int64_t> keyframes;
		std::error_code ec;
		const auto file_size = std::filesystem::file_size(file_path, ec);
		const auto cache_hit = !ec && !kairos_keyframes_ms_.empty()
			&& kairos_keyframes_size_ == static_cast<int64_t>(file_size)
			&& kairos_keyframes_mtime_ == statMtimeEpochSecs(file_path);
		if (cache_hit)
		{
			keyframes = kairos_keyframes_ms_;
		}
		else
		{
			keyframes = probeKeyframeTimestampsMs(opts.ffprobe_path, file_path);
			if (!keyframes.empty() && !ec)
			{
				const int64_t mtime  = statMtimeEpochSecs(file_path);
				const std::string ct = content_type_, cid = content_id_;
				const int64_t sz     = static_cast<int64_t>(file_size);
				// Captures the manager, not `this` — manager_ outlives every
				// VodSession it hands out (see its own header comment), but
				// this session could be stopped/destroyed before a detached
				// background task gets to run.
				VodSessionManager* mgr = &manager_;
				TaskRegistry::global().spawn([mgr, ct, cid, keyframes, sz, mtime]
				{
					mgr->pushKeyframeCache(ct, cid, keyframes, sz, mtime);
				});
			}
		}
		segment_start_ms = simulateDirectStreamSegmentBoundaries(keyframes, kVodHlsSegmentSecs);
		if (segment_start_ms.empty())
			std::cerr << "[vod:" << session_id << "] keyframe probe failed/empty for direct-stream — "
			             "falling back to assumed uniform segment cadence (segment boundaries may drift from actual cut points)\n";
		// segment_start_ms[0] is what the HLS timeline (and therefore hls.js)
		// treats as "elapsed 0" for the video. The subtitle pipe's cues, in
		// contrast, always use the sidecar/embedded track's own raw
		// timestamps starting from true file position 0 (buildVodSubtitleArgs
		// never applies -ss). If the file's first real keyframe isn't at
		// exactly 0 (common with encoder priming/B-frame delay on some
		// sources), those two "0" points diverge by that amount, and
		// subtitles would appear to lead the video by a constant offset —
		// this line exists to confirm/rule that out directly.
		else if (!segment_start_ms.empty() && segment_start_ms[0] != 0)
			std::cerr << "[vod:" << session_id << "] direct-stream first keyframe at "
			          << segment_start_ms[0] << "ms, not 0 — video's HLS-timeline zero and the "
			             "subtitle pipe's file-relative zero will diverge by that much\n";
	}
	if (segment_start_ms.empty())
	{
		// Transcode/burn-in (keyframes forced at exactly this cadence via
		// -force_key_frames) or a failed direct-stream keyframe probe.
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
		// affects direct-stream (no decode of video/audio involved).
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
		if (track == audio_track_) return true; // already active — no-op

		std::cerr << "[vod:" << session_id << "] ensureAudioTrack SWITCHING audio_track_ " << audio_track_
				  << " -> " << track << "\n";
		audio_track_ = track;
		// Video is untouched by an audio track switch now — the two are
		// independent streams (see isVideoDirectStreamable/isAudioDirectStreamable's
		// shared comment), so this no longer needs to recompute
		// segment_start_ms/direct_stream or touch video_stream_ at all. Just
		// swap audio_stream_ for a fresh one targeting the new track.
		int target_segment = std::clamp(last_requested_segment.load(), 0, total_segments - 1);
		rebuildAudioStream(target_segment);
		return true;
	}
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
// thread on the audio-track-thrashing bug this fixes). direct_stream here only
// gates the VIDEO half now — video and audio are independent streams (see
// isVideoDirectStreamable/isAudioDirectStreamable), so each audio track's own
// eligibility is re-checked individually below rather than assumed from
// video's. Returns nullopt (omit CODECS entirely) the moment ANY piece can't
// be confidently derived — video not direct-streamable, or even one audio
// track either not directly playable itself (its real transcoded output
// codec/profile isn't something media_info, the SOURCE probe, describes) or
// unrecognized — all-or-nothing, since a partial CODECS list is arguably
// worse than none.
std::optional<std::string> VodSession::buildCodecsAttribute() const {
	if (!direct_stream) return std::nullopt;
	std::vector<std::string> parts;
	if (!media_info.video.empty()) {
		auto vcodec = h264CodecString(media_info.video[0]);
		if (!vcodec) return std::nullopt;
		parts.push_back(*vcodec);
	}
	std::set<std::string> distinct_audio;
	for (auto& a : media_info.audio)
	{
		if (!isAudioDirectStreamable(media_info, a.relative_index, client_caps_)) return std::nullopt;
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
	// can derive it confidently (direct-stream + recognized codecs) — see its
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

// Shared by both writeVideoPlaylist()/writeAudioPlaylist() — video and audio
// always agree on segment_start_ms/total_segments (see the class comment on
// why), so the only difference between the two playlists is which filename
// prefix each #EXTINF entry points at. No #EXT-X-DISCONTINUITY anywhere:
// every head (video or audio) is started with -output_ts_offset rebasing its
// output onto the absolute file timeline (see pushHeadBoundArgs), so segment
// N+1 from a different head than segment N still carries continuous
// timestamps — there's no real discontinuity left to signal.
std::string VodSession::buildStaticPlaylist(const std::string& segment_prefix) const
{
	// #EXT-X-TARGETDURATION must be an upper bound (rounded up) on every
	// segment's actual duration per spec — direct-stream segments can run
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
		int64_t seg_start = segment_start_ms[static_cast<size_t>(i)];
		int64_t seg_end = (i + 1 < total_segments) ? segment_start_ms[static_cast<size_t>(i + 1)] : effective_duration_ms;
		double dur_secs = std::max(0.1, (seg_end - seg_start) / 1000.0);

		char extinf[32];
		std::snprintf(extinf, sizeof(extinf), "%.3f", dur_secs);
		out << "#EXTINF:" << extinf << ",\n";

		char seg[32];
		std::snprintf(seg, sizeof(seg), "%s%05d.ts", segment_prefix.c_str(), i);
		out << seg << "\n";
	}

	out << "#EXT-X-ENDLIST\n";
	return out.str();
}

// Caller must hold session_mtx.
void VodSession::writeVideoPlaylist() const {
	std::ofstream f(dir() + "/playlist.m3u8", std::ios::trunc);
	f << buildStaticPlaylist("seg-");
}

// Caller must hold session_mtx.
void VodSession::writeAudioPlaylist() const {
	std::ofstream f(dir() + "/audio-playlist.m3u8", std::ios::trunc);
	f << buildStaticPlaylist("aseg-");
}

VodSession::SegmentPrep VodSession::prepareSegment(int segment_index) {
	if (!active.load()) return SegmentPrep::Failed;
	last_requested_segment.store(segment_index);
	// Snapshot the shared_ptr under session_mtx rather than dereferencing
	// video_stream_ directly — this runs on whatever thread is handling the
	// HTTP segment request, concurrently with ensureAudioTrack()/stop() on
	// another thread, both of which mutate video_stream_/audio_stream_ under
	// this same lock. An unguarded read here was racing against those
	// writes (a plain shared_ptr isn't safe to read on one thread while
	// another reassigns/resets it, same as any other unsynchronized
	// variable) — session_mtx's own comment already documents it as the
	// lock for exactly this pair of fields, this just wasn't taking it.
	// Not held for the prepareSegment() call itself below (which can spawn/
	// wait on ffmpeg) so it doesn't serialize behind a potentially slow
	// segment request.
	std::shared_ptr<VodEncodeStream> stream;
	{
		std::lock_guard<std::mutex> lock(session_mtx);
		stream = video_stream_;
	}
	if (!stream) return SegmentPrep::Failed;
	auto prep = stream->prepareSegment(segment_index, segment_start_ms, total_segments);
	switch (prep) {
		case VodEncodeStream::SegmentPrep::Ready:        return SegmentPrep::Ready;
		case VodEncodeStream::SegmentPrep::WaitShort:     return SegmentPrep::WaitShort;
		case VodEncodeStream::SegmentPrep::WaitColdStart: return SegmentPrep::WaitColdStart;
		default:                                          return SegmentPrep::Failed;
	}
}

// Same contract as prepareSegment(), for the independent audio stream — see
// Router.cpp's /audio/{n}/seg-{n}.ts route. Same session_mtx snapshot
// reasoning as prepareSegment() above — audio_stream_ is the one that
// actually gets reassigned mid-session (ensureAudioTrack(), on a track
// switch), so this was the read most likely to actually race in practice.
VodSession::SegmentPrep VodSession::prepareAudioSegment(int segment_index) {
	if (!active.load()) return SegmentPrep::Failed;
	std::shared_ptr<VodEncodeStream> stream;
	{
		std::lock_guard<std::mutex> lock(session_mtx);
		stream = audio_stream_;
	}
	if (!stream) return SegmentPrep::Failed;
	auto prep = stream->prepareSegment(segment_index, segment_start_ms, total_segments);
	switch (prep) {
		case VodEncodeStream::SegmentPrep::Ready:        return SegmentPrep::Ready;
		case VodEncodeStream::SegmentPrep::WaitShort:     return SegmentPrep::WaitShort;
		case VodEncodeStream::SegmentPrep::WaitColdStart: return SegmentPrep::WaitColdStart;
		default:                                          return SegmentPrep::Failed;
	}
}

void VodSession::lookaheadLoop() {
	while (!lookahead_stop.load() && active.load()) {
		std::this_thread::sleep_for(std::chrono::seconds(kLookaheadTickSecs));
		if (!active.load() || lookahead_stop.load()) return;
		// Same session_mtx snapshot as prepareSegment()/prepareAudioSegment()
		// above — this runs on its own dedicated thread, so an unguarded
		// read here raced against ensureAudioTrack()/stop() exactly the same
		// way theirs did.
		std::shared_ptr<VodEncodeStream> vstream, astream;
		{
			std::lock_guard<std::mutex> lock(session_mtx);
			vstream = video_stream_;
			astream = audio_stream_;
		}
		if (vstream) vstream->tick(total_segments);
		if (astream) astream->tick(total_segments);
	}
}

void VodSession::stop() {
	if (!active.exchange(false)) return;

	lookahead_stop = true;
	if (lookahead_thread.joinable()) lookahead_thread.join();

	{
		std::lock_guard<std::mutex> lock(session_mtx);
		// .reset(), not ->stop() — these are shared_ptrs onto content-keyed
		// streams another viewer may still be actively referencing (see
		// VodSessionManager.h's class comment). Ordinary refcounting tears a
		// stream down (VodEncodeStream::~VodEncodeStream calls its own
		// stop()) only once the last VodSession pointing at it lets go.
		video_stream_.reset();
		audio_stream_.reset();
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
