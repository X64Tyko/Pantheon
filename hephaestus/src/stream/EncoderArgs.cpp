#include "EncoderArgs.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

std::string fmtSpeed(double speed)
{
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(4) << speed;
	return ss.str();
}

void pushHeadBoundArgs(std::vector<std::string>& a, int64_t positionMs, std::optional<double> windowDurationSecs)
{
	std::ostringstream offset;
	offset << std::fixed << std::setprecision(3) << (positionMs / 1000.0);
	a.insert(a.end(), {"-output_ts_offset", offset.str()});
	if (windowDurationSecs)
	{
		std::ostringstream dur;
		dur << std::fixed << std::setprecision(3) << *windowDurationSecs;
		a.insert(a.end(), {"-t", dur.str()});
	}
}

std::vector<int64_t> simulateDirectStreamSegmentBoundaries(
	const std::vector<int64_t>& keyframes_ms, int hls_time_secs)
{
	std::vector<int64_t> bounds;
	if (keyframes_ms.empty()) return bounds;
	const int64_t hls_time_ms = int64_t(hls_time_secs) * 1000;
	int64_t current_start     = keyframes_ms.front(); // should be ~0
	bounds.push_back(current_start);
	int64_t target = current_start + hls_time_ms;
	for (int64_t kf : keyframes_ms)
	{
		if (kf <= current_start) continue;
		if (kf >= target)
		{
			bounds.push_back(kf);
			current_start = kf;
			target        = current_start + hls_time_ms;
		}
	}
	return bounds;
}

const char* hwAccelName(HwAccel hw_accel)
{
	switch (hw_accel)
	{
		case HwAccel::nvidia: return "nvidia";
		case HwAccel::amd: return "amd";
		default: return "none";
	}
}

std::string decodeCodecKey(const std::string& codec, int bit_depth)
{
	if (codec == "h264" || bit_depth <= 8) return codec;
	return codec + std::to_string(bit_depth);
}

void pushVaapiDeviceArg(std::vector<std::string>& a, HwAccel encode, HwAccel decode,
						const std::string& vaapi_device)
{
	if (encode == HwAccel::amd || decode == HwAccel::amd) a.insert(a.end(), {"-vaapi_device", vaapi_device});
}

void pushHwAccelDecodeArgs(std::vector<std::string>& a, HwAccel decode_backend,
						   const std::set<std::string>& decodable_codecs,
						   const std::string& source_codec)
{
	if (decode_backend == HwAccel::none) return;
	if (source_codec.empty() || !decodable_codecs.count(source_codec)) return;
	if (decode_backend == HwAccel::nvidia) a.insert(a.end(), {"-hwaccel", "cuda"});
	else if (decode_backend == HwAccel::amd) a.insert(a.end(), {"-hwaccel", "vaapi"});
}

// ffprobe and zscale/ffmpeg's output color options share the same enum
// names (both come from ffmpeg's own AVColor* enums), so a probed value can
// be passed straight through to either without translation. Falls back to
// the overwhelmingly common HDR10 triple (bt2020/bt2020nc/smpte2084) for any
// field ffprobe didn't report, rather than leaving it unset — zscale in
// particular fails outright ("no path between colorspaces") given a
// half-specified input colorspace instead of guessing.
std::string orDefaultColorTag(const std::string& v, const char* def)
{
	return v.empty() ? def : v;
}

// Real HDR (PQ/HLG, BT.2020) -> SDR (BT.709) conversion, not just a metadata
// relabel. Verified end-to-end against a real ffmpeg n8.1.1 build (libzimg):
// zscale's auto-detection of embedded frame colorspace is unreliable enough
// on real-world files (partial/missing tags, common on older or badly-muxed
// rips) that leaving transferin/primariesin/matrixin unset intermittently
// fails with "no path between colorspaces" — passing the values MediaProbe
// already read from ffprobe explicitly makes this deterministic regardless
// of how complete the source's own tagging is.
//
// hable+desat=0 (no automatic desaturation of bright highlights) is a
// deliberately plain, content-agnostic tone-map operator — no per-title
// tuning here, this pipeline has no concept of per-file mastering metadata
// beyond the VUI tags themselves.
std::string buildHdrToSdrTonemapFilter(const VideoTrack& source_video)
{
	std::string transfer  = orDefaultColorTag(source_video.color_transfer, "smpte2084");
	std::string primaries = orDefaultColorTag(source_video.color_primaries, "bt2020");
	std::string matrix    = orDefaultColorTag(source_video.color_space, "bt2020nc");

	return "zscale=transferin=" + transfer + ":primariesin=" + primaries + ":matrixin=" + matrix +
		":transfer=linear:primaries=" + primaries + ":matrix=" + matrix + ":npl=100"
		",format=gbrpf32le"
		",zscale=primaries=bt709"
		",tonemap=tonemap=hable:desat=0"
		",zscale=transfer=bt709:matrix=bt709:primaries=bt709:range=tv"
		",format=yuv420p";
}

const std::vector<VideoCodecOption>& videoCodecPriority()
{
	static const std::vector<VideoCodecOption> options = {
		{
			"hevc", [](std::vector<std::string>& a, std::vector<std::string>& vfParts, HwAccel hw_accel)
			{
				switch (hw_accel)
				{
					case HwAccel::nvidia: a.insert(a.end(), {
													   "-c:v", "hevc_nvenc", "-preset", "p4",
													   "-rc:v", "vbr", "-cq", "23", "-pix_fmt", "yuv420p",
													   "-forced-idr", "1",
													   "-no-scenecut", "1" // see the h264 entry below for why
												   });
						break;
					case HwAccel::amd: vfParts.push_back("format=nv12,hwupload");
						a.insert(a.end(), {"-c:v", "hevc_vaapi"});
						break;
					default: a.insert(a.end(), {
										  "-c:v", "libx265", "-preset", "veryfast",
										  "-crf", "23", "-pix_fmt", "yuv420p",
										  "-sc_threshold", "0" // see the h264 entry below for why
									  });
				}
			}
		},
		{
			"h264", [](std::vector<std::string>& a, std::vector<std::string>& vfParts, HwAccel hw_accel)
			{
				switch (hw_accel)
				{
					case HwAccel::nvidia: a.insert(a.end(), {
													   "-c:v", "h264_nvenc", "-preset", "p4",
													   "-rc:v", "vbr", "-cq", "23", "-pix_fmt", "yuv420p",
													   // Without this, h264_nvenc can silently treat a
													   // -force_key_frames request as an ordinary frame
													   // its own internal GOP logic is still free to
													   // reorder/skip, falling back to its default
													   // ~250-frame GOP instead of the caller's
													   // keyframeIntervalSecs -- HLS segments then land
													   // at that default interval (e.g. 10.4s at 24fps)
													   // instead of near -hls_time. Confirmed against a
													   // real VOD session: #EXTINF was landing at
													   // exactly 250 frames' worth of playback time.
													   // -forced-idr makes NVENC emit a true IDR frame
													   // for every forced keyframe instead.
													   "-forced-idr", "1",
													   // NVENC has its own scene-cut heuristic that
													   // -force_key_frames does NOT disable, so a real
													   // scene change landing between two forced keyframes
													   // can still get an extra, unplanned one on top —
													   // irregular GOP structure the segmenter isn't
													   // expecting. The obvious fix, -sc_threshold, is
													   // silently ignored by h264_nvenc/hevc_nvenc
													   // (confirmed: https://forums.developer.nvidia.com/
													   // t/ffmpeg-encoder-h264-nvenc-ignores-sc-threshold-
													   // value/212726) — -no-scenecut is the flag that
													   // actually works for this encoder family.
													   "-no-scenecut", "1"
												   });
						break;
					case HwAccel::amd: vfParts.push_back("format=nv12,hwupload");
						a.insert(a.end(), {"-c:v", "h264_vaapi"});
						break;
					default: a.insert(a.end(), {
										  "-c:v", "libx264", "-preset", "veryfast",
										  "-crf", "23", "-pix_fmt", "yuv420p",
										  // libx264/libx265 (unlike NVENC) genuinely respect
										  // -sc_threshold — same "no unplanned extra keyframes
										  // between our own forced ones" reasoning as -no-scenecut
										  // above, just the correct flag for this encoder family.
										  "-sc_threshold", "0"
									  });
				}
			}
		},
	};
	return options;
}

// Picks the best codec the client has actually declared support for,
// bounded by the source's own position in the priority list — never more
// "exotic"/modern than the source already is (e.g. a HEVC source never
// transcodes to some hypothetical future AV1 target even if the client
// supports it). Re-encoding UP a generation buys nothing real: the source
// was never encoded with the extra quality/efficiency headroom a newer
// codec offers, so all that changes is more encode time for zero benefit.
// An unrecognized/legacy source (not in this list at all, e.g. mpeg2/vc1)
// has no such bound — nothing suggests a modern target wouldn't help it, so
// the full list is fair game. Falls back to the last (most universally
// compatible) entry if client_caps is unset or matches nothing above the
// bound — the same role the fixed H.264 target unconditionally played
// before this existed.
const VideoCodecOption& chooseVideoCodec(const std::string& source_codec,
										 const std::optional<ClientCapabilities>& client_caps)
{
	const auto& options = videoCodecPriority();
	size_t source_tier  = 0; // unrecognized source = no bound
	for (size_t i = 0; i < options.size(); ++i)
	{
		if (options[i].name == source_codec)
		{
			source_tier = i;
			break;
		}
	}
	if (client_caps)
	{
		for (size_t i = source_tier; i < options.size(); ++i)
		{
			if (client_caps->video_codecs.count(options[i].name)) return options[i];
		}
	}
	return options.back();
}

void pushVideoEncoderArgs(std::vector<std::string>& a, std::vector<std::string>& vfParts,
						  HwAccel hw_accel, int keyframeIntervalSecs,
						  const VideoTrack* source_video, bool client_hdr_capable,
						  const std::optional<ClientCapabilities>& client_caps)
{
	bool source_hdr      = source_video && isHdrTransfer(source_video->color_transfer);
	bool passthrough_hdr = source_hdr && client_hdr_capable;

	// GOP-size hint matching the real forced-keyframe cadence below. Without
	// this, a hardware encoder plans its internal GOP/rate-control structure
	// around its own default GOP length (NVENC's is far longer than a live
	// HLS segment — see the h264 branch's own "-forced-idr" comment for the
	// sibling bug this is related to), then has to interrupt/replan that
	// structure every single time -force_key_frames actually fires — a real,
	// periodic cost landing exactly on the forced-keyframe interval, which
	// can show up as a repeating stutter synced to it. Telling the encoder
	// the real cadence upfront (in frames, not seconds) lets it plan for it
	// instead of being repeatedly surprised. Skipped entirely if the
	// source's frame rate can't be determined — a wrong guess here is worse
	// than leaving the encoder's own default alone.
	double fps     = source_video ? parseFrameRateFraction(source_video->r_frame_rate) : 0.0;
	int gop_frames = fps > 0.0 ? static_cast<int>(std::lround(fps * keyframeIntervalSecs)) : 0;

	if (source_hdr && !passthrough_hdr) vfParts.insert(vfParts.begin(), buildHdrToSdrTonemapFilter(*source_video));

	// Client-declared resolution cap — already a plain pixel height, not the
	// "1080p"-style string resolveMaxHeight parses (that's for a discrete
	// settings dropdown; max_height here comes directly off the client's own
	// declaration). No-op (min() inside the filter) if source is already
	// at or under it.
	if (client_caps && client_caps->max_height) pushScaleFilter(vfParts, *client_caps->max_height);

	// Real HDR10 passthrough: re-encode at true 10-bit instead of downgrading
	// to 8-bit/BT.709, for a client that already told us (via
	// client_hdr_capable — see Router.cpp's session-start handlers) its
	// display can actually show the extra range. H.264 has no real-world
	// HDR10 story, so this is HEVC Main10 across every backend, verified
	// against a real ffmpeg n8.1.1 build (libx265: Main 10 profile,
	// yuv420p10le, color_space round-tripped correctly through an .m3u8/.ts
	// HLS segment). NVENC/VAAPI 10-bit paths follow the same shape as their
	// existing 8-bit branches below but couldn't be verified end-to-end here
	// (no GPU in this environment) — pix_fmt choice per backend (p010le for
	// hardware surfaces vs yuv420p10le for libx265) matches each encoder's
	// own supported-pixel-format list (`ffmpeg -h encoder=<name>`).
	//
	// Deliberately does NOT attempt to carry over the source's static HDR10
	// metadata (Mastering Display Color Volume / MaxCLL/MaxFALL SEI) — real
	// benefit (a receiver's own tone-mapping tuned to the actual mastering
	// range) but meaningfully more work to extract via ffprobe and re-inject
	// via -master_display/-max_cll, and most receivers fall back to sane
	// generic assumptions without it. Follow-up if displays look off without it.
	if (passthrough_hdr)
	{
		switch (hw_accel)
		{
			case HwAccel::nvidia: a.insert(a.end(), {
											   "-c:v", "hevc_nvenc", "-preset", "p4", "-profile:v", "main10",
											   "-rc:v", "vbr", "-cq", "23", "-pix_fmt", "p010le",
											   "-forced-idr", "1",
											   "-no-scenecut", "1" // see the 8-bit nvenc branch below for why
										   });
				break;
			case HwAccel::amd: vfParts.push_back("format=p010le,hwupload");
				a.insert(a.end(), {"-c:v", "hevc_vaapi", "-profile:v", "main10"});
				break;
			default: a.insert(a.end(), {
								  "-c:v", "libx265", "-preset", "veryfast", "-crf", "23",
								  "-pix_fmt", "yuv420p10le",
								  // x265-params takes scenecut, not a top-level ffmpeg option,
								  // for this encoder — same "no unplanned extra keyframes
								  // between our own forced ones" reasoning as the 8-bit branch.
								  "-x265-params", "hdr10=1:repeat-headers=1:scenecut=0"
							  });
		}
		a.insert(a.end(), {
					 "-force_key_frames",
					 "expr:gte(t,n_forced*" + std::to_string(keyframeIntervalSecs) + ")"
				 });
		if (gop_frames > 0) a.insert(a.end(), {"-g", std::to_string(gop_frames)});
		// The source's *actual* color info, not bt709 — this is real HDR
		// output, unlike the forced-bt709 tagging in the SDR path below.
		a.insert(a.end(), {
					 "-color_primaries", orDefaultColorTag(source_video->color_primaries, "bt2020"),
					 "-color_trc", orDefaultColorTag(source_video->color_transfer, "smpte2084"),
					 "-colorspace", orDefaultColorTag(source_video->color_space, "bt2020nc"),
				 });
		return;
	}

	// Smart mux — see chooseVideoCodec()'s own comment. This is NOT the
	// HDR10 Main10 path above (that's real 10-bit HDR, always HEVC); this is
	// still plain SDR content, only the transcode target varies now.
	chooseVideoCodec(source_video ? source_video->codec : "", client_caps)
		.buildArgs(a, vfParts, hw_accel);
	a.insert(a.end(), {
				 "-force_key_frames",
				 "expr:gte(t,n_forced*" + std::to_string(keyframeIntervalSecs) + ")"
			 });
	if (gop_frames > 0) a.insert(a.end(), {"-g", std::to_string(gop_frames)});

	// Every branch above targets plain 8-bit yuv420p output regardless of
	// source, so the output is always structurally BT.709 SDR now — for an
	// HDR source, that's only true *pixel data* because of the tonemap
	// filter chain inserted above; without it, ffmpeg's automatic pix_fmt
	// conversion correctly truncates bit depth but leaves un-tone-mapped
	// PQ-range values in an 8-bit container while copying the *color tags*
	// through unchanged, producing output that's structurally SDR but whose
	// VUI metadata still claims BT.2020/SMPTE ST 2084 (PQ) HDR. Confirmed via
	// ffprobe against a real transcoded segment before the tonemap chain
	// existed: pix_fmt=yuv420p but color_transfer=smpte2084, color_primaries=
	// bt2020, color_space=bt2020nc. Browsers that respect embedded HDR
	// signaling (common on hardware-accelerated decode paths) can silently
	// fail to render that mismatch (no fatal error surfaces to hls.js/JS,
	// playback just never starts), and displays that do play it show
	// washed-out, hazy, crushed-contrast video (PQ-range values decoded
	// through a standard gamma EOTF). Retagging as bt709 unconditionally
	// here is what makes the declared color space match what the pixel data
	// actually is in both cases: genuinely SDR source untouched, HDR source
	// now genuinely tone-mapped to SDR above.
	a.insert(a.end(), {"-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709"});
}

const std::vector<AudioCodecOption>& audioCodecPriority()
{
	static const std::vector<AudioCodecOption> options = {
		// 384k: a standard AC3/EAC3 5.1 rate with real headroom even if the
		// source is actually 7.1 — audio_bitrate_kbps isn't reused here
		// since every caller tunes that specifically for stereo AAC.
		{"eac3", true, 384},
		{"ac3", true, 384},
		{"aac", false, 0}, // universal fallback — same role it played unconditionally before this existed
	};
	return options;
}

// Smart mux, bounded by the source the same way chooseVideoCodec() is —
// here the bound is channel count, not codec generation: a surround-capable
// codec buys nothing for a source that's already stereo/mono, so those
// never enter the running regardless of what the client declares. Only once
// there's real surround content to preserve does a client's eac3/ac3
// support matter at all.
const AudioCodecOption& chooseAudioCodec(const AudioTrack* source_audio,
										 const std::optional<ClientCapabilities>& client_caps)
{
	const auto& options      = audioCodecPriority();
	bool source_has_surround = source_audio && source_audio->channels > 2;
	if (source_has_surround && client_caps)
	{
		for (auto& opt : options)
		{
			if (opt.preserve_channels && client_caps->audio_codecs.count(opt.name)) return opt;
		}
	}
	return options.back(); // aac
}

void pushAudioEncoderArgs(std::vector<std::string>& a, bool loudnorm, double speed,
						  int audio_bitrate_kbps,
						  const std::optional<ClientCapabilities>& client_caps,
						  const AudioTrack* source_audio,
						  bool debug_showinfo,
						  bool resync_audio)
{
	std::vector<std::string> afParts;
	// Placed first in the chain, before loudness/speed shaping — resync
	// against the source's own timestamps, then apply everything else on top
	// of an already-aligned stream. See this function's own header comment
	// for why this only applies to the direct-stream/native bucket's
	// stream-copied-video pairing.
	if (resync_audio) afParts.push_back("aresample=async=1");
	// dynaudnorm, not loudnorm: loudnorm's single-pass mode is a real-time
	// estimate with no knowledge of the whole stream, and ffmpeg's own docs
	// note it "shouldn't be used for VOD" for that reason (see VodSession's
	// loudnorm=false) — the same instability shows up as audible drift over
	// a channel's continuous, hours-long runtime. dynaudnorm re-normalizes
	// on short (<8s) windows instead of one long-running estimate, which is
	// what ffmpeg recommends for exactly this always-on streaming case.
	// Plain dynaudnorm only targets peak level (p=0.95 by default) with
	// targetrms disabled -- most library content already peaks near full
	// scale, so peak-only normalization applies almost no gain and sounds
	// much quieter than loudnorm's old -16 LUFS target did. r= turns on
	// RMS-based targeting so quiet passages actually get boosted; m= raises
	// the max gain factor so there's enough headroom left to reach it.
	if (loudnorm) afParts.push_back("dynaudnorm=m=15:r=0.2");
	if (speed != 1.0) afParts.push_back("atempo=" + fmtSpeed(speed));

	const auto& codec = chooseAudioCodec(source_audio, client_caps);
	int bitrate_kbps  = codec.bitrate_kbps > 0 ? codec.bitrate_kbps : audio_bitrate_kbps;
	if (codec.preserve_channels)
	{
		// Plain AC3 (Dolby Digital) is spec-capped at 5.1 (6 channels) — its
		// bitstream format has no way to carry more. E-AC-3 (Dolby Digital
		// Plus) is the one that actually supports up to 7.1 (8 channels);
		// "preserve_channels" alone isn't enough for ac3 specifically, or a
		// genuinely 7.1 source would hand ffmpeg's ac3 encoder more channels
		// than the format allows. No cap for eac3 (or anything else in this
		// list) — letting ffmpeg carry through whatever channel count the
		// source decodes to is the entire point there.
		if (codec.name == "ac3" && source_audio && source_audio->channels > 6) a.insert(a.end(), {"-c:a", codec.name, "-ac", "6", "-b:a", std::to_string(bitrate_kbps) + "k"});
		else a.insert(a.end(), {"-c:a", codec.name, "-b:a", std::to_string(bitrate_kbps) + "k"});
	}
	else
	{
		// Force stereo: without an explicit -ac, ffmpeg preserves the source's
		// channel count (e.g. a 5.1/7.1 theatrical mix), and browsers' MSE
		// SourceBuffer support for multichannel AAC is far less reliable than
		// for stereo -- if the audio SourceBuffer never successfully
		// initializes, playback can stall entirely even though video decodes
		// fine (MSE generally wants every active SourceBuffer ready before
		// playback starts). Nothing watching through a browser tab does true
		// surround passthrough anyway, so downmixing here costs nothing real.
		a.insert(a.end(), {"-c:a", "aac", "-ac", "2", "-b:a", std::to_string(bitrate_kbps) + "k"});
	}
	// Last in the chain: reports pts_time after any dynaudnorm/atempo filter
	// above has already run, i.e. the timing actually handed to the encoder.
	if (debug_showinfo) afParts.push_back("ashowinfo");

	if (!afParts.empty())
	{
		std::string af;
		for (size_t i = 0; i < afParts.size(); ++i)
		{
			if (i) af += ",";
			af += afParts[i];
		}
		a.insert(a.end(), {"-af", af});
	}
}

void pushVideoFilterArgs(std::vector<std::string>& a, const std::vector<std::string>& vfParts)
{
	if (vfParts.empty()) return;
	std::string vf;
	for (size_t i = 0; i < vfParts.size(); ++i)
	{
		if (i) vf += ",";
		vf += vfParts[i];
	}
	a.insert(a.end(), {"-vf", vf});
}

int resolveMaxHeight(const std::string& max_resolution)
{
	if (max_resolution == "1080p") return 1080;
	if (max_resolution == "720p") return 720;
	if (max_resolution == "480p") return 480;
	return 0; // "source" or unrecognized
}

void pushScaleFilter(std::vector<std::string>& vfParts, int maxHeight)
{
	if (maxHeight <= 0) return;
	vfParts.push_back("scale=-2:min(ih\\," + std::to_string(maxHeight) + ")");
}

void pushBitrateCapArgs(std::vector<std::string>& a, int video_bitrate_kbps)
{
	if (video_bitrate_kbps <= 0) return;
	std::string maxrate = std::to_string(video_bitrate_kbps) + "k";
	std::string bufsize = std::to_string(video_bitrate_kbps * 2) + "k";
	a.insert(a.end(), {"-maxrate", maxrate, "-bufsize", bufsize});
}

int defaultBitrateCapKbps(int effective_height)
{
	if (effective_height <= 0) return 8000; // unknown — assume ~1080p-ish
	if (effective_height <= 480) return 4000;
	if (effective_height <= 720) return 6000;
	if (effective_height <= 1080) return 10000;
	return 20000; // >1080p (4K etc.)
}

int effectiveOutputHeight(int max_height_cap, const VideoTrack* source_video)
{
	int source_height = source_video ? source_video->height : 0;
	if (max_height_cap <= 0) return source_height;
	if (source_height <= 0) return max_height_cap;
	return std::min(max_height_cap, source_height);
}

void pushLogLevelArgs(std::vector<std::string>& a, bool verbose_transcode_logs)
{
	// -stats_period default is 0.5s — far chattier than useful for a
	// long-running live channel; 2s still gives fine-grained enough
	// frame/fps/speed/drop/dup counters to correlate against a reported drift
	// window without flooding the log. `-stats` isn't implied by `-v verbose`
	// (they're independent ffmpeg switches — verbose raises the log *level*,
	// stats is a separate periodic progress line ffmpeg only prints
	// automatically when stderr is a tty, which it never is here since it's
	// piped to FfmpegProcess), so it needs to be requested explicitly to show
	// up at all in a piped/Docker deployment.
	if (verbose_transcode_logs) a.insert(a.end(), {"-v", "verbose", "-stats", "-stats_period", "2"});
}