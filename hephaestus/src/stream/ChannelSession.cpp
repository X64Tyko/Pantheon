#include "ChannelSession.h"
#include "EncoderArgs.h"
#include "MediaProbe.h"
#include "thread/TaskRegistry.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <cmath>

using clock_t_ = std::chrono::system_clock;
using steady_  = std::chrono::steady_clock;

// How long start() waits for the initial /now lookup before falling back to
// the splash. Kairos normally answers in low single-digit ms — far faster
// than ffmpeg can fork/exec/init — so unconditionally spawning the splash
// first (the original design) meant it was always killed and replaced before
// it ever produced a frame: zero visible benefit, plus a wasted spawn. Only
// engage the splash for genuine cold-start/network slowness, which is what
// Hermes's ChannelBroadcaster retry/backoff already budgets several seconds for.
static constexpr auto kFastPathBudget = std::chrono::milliseconds(250);

static int64_t nowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		clock_t_::now().time_since_epoch()).count();
}

// Drift-to-speed correction tuning. Kept conservative/near-imperceptible:
// only ever nudge playback by up to 2%, and only attempt it when the drift
// is small enough that a 2% nudge over the item's own duration is actually
// enough to close it — otherwise fall back to seeking/skipping content.
static constexpr double kMinSpeed = 0.98;
static constexpr double kMaxSpeed = 1.02;

// Shared by appendOutputArgs' -hls_time and pushVideoEncoderArgs'
// -force_key_frames so the two can never drift apart the way they did
// before. Only the transcode bucket can actually honor this, though — a
// direct-stream (kNativeBucket) `-c:v copy` session can't force keyframes
// into a copy, so real segment length there is whatever the source file's
// own GOP spacing is (can be 5-15s+). kLiveHlsListSize/kLiveHlsDeleteThreshold
// below are sized generously to absorb that case rather than just the
// intended segment length.
//
// KNOWN TRADEOFF, ROOT CAUSE STILL UNRESOLVED — this value has flip-flopped
// between 2 and 6 as the two symptoms it affects took turns being the
// priority:
//   - At 2: a live A/B/A test confirmed a periodic transcode-bucket stutter
//     during playback is tied to this interval (2s glitches, 6s doesn't, 2s
//     glitches again). The leading theory — missing `-g` letting the encoder
//     replan its internal GOP structure every forced keyframe — was
//     implemented (EncoderArgs.cpp sizes `-g` off the source's real frame
//     rate) but confirmed NOT sufficient on its own: the glitch still
//     reproduces at 2s with that fix in place. Reproduced on real
//     (software-only, no-hwaccel) hardware, ruling out a GPU-specific cause.
//   - At 6: cold-start/channel-switch latency gets worse — ffmpeg (paced by
//     -re) only closes/flushes the first segment once hls_time seconds of
//     real wall-clock content has played, so a slow tune-in more easily loses
//     the race against the client's manifest-load patience (Router.cpp's
//     waitForFile) — a plausible contributor to slow/failed channel
//     transitions.
// Set back to 2 (2026-08-05) to prioritize transition/tune-in latency; if the
// periodic in-stream stutter reappears, that's the expected tradeoff, not a
// new regression — see project memory for the full investigation history
// before re-flipping this again.
static constexpr int kLiveHlsSegmentSecs = 2;

// Oversized vs. the 2s target so a direct-stream session's much-longer real
// segments (see above) still get a safe rolling window instead of a client's
// slightly-stale manifest racing a too-eager delete. Blunt fix, not the
// precise one (which would size -hls_time per item off its real keyframe
// spacing, already cached in keyframes_ms) — good follow-up, not done here.
static constexpr int kLiveHlsListSize        = 12;
static constexpr int kLiveHlsDeleteThreshold = 8;

// How long transition()'s own default-slate fallback runs before rechecking
// Kairos, when /now returns nothing at all (no scheduled item, no filler,
// no offline config) — mirrors Kairos's own kOfflineRecheckMs
// (SchedulerService.cpp) for a real offline item's wall_clock_end_ms; this
// is the local equivalent for the case where Kairos gave us nothing to
// compute one from.
static constexpr int64_t kNoContentRetryMs = 20'000;

// How long before the current item's scheduled end prefetchLoop() starts
// warming the next item's probe cache. Long enough that a slow probe
// (network share, large file) has finished well before transition() actually
// spawns ffmpeg for it; short enough that a short filler/bumper isn't
// already over before this even fires.
static constexpr int64_t kPrefetchLeadMs = 8'000;

// How often prefetchLoop() checks whether it's inside the lead window —
// coarser than hlsPatchLoop's cadence since this is a lookahead, not a
// per-segment correction, and cheap headroom against a 20s+ lead is fine.
static constexpr int kPrefetchTickSecs = 2;

// ── ffmpeg arg construction ───────────────────────────────────────────────────
// pushVideoEncoderArgs()/pushAudioEncoderArgs()/fmtSpeed() live in
// EncoderArgs.h/.cpp so VodSession can share them too.

// Appends the final output-format arguments. When hls_dir is non-empty, uses
// ffmpeg's tee muxer to duplicate the single encode to both the existing
// MPEG-TS stdout pipe (Hermes fan-out / DVR clients, unchanged) and a rolling
// HLS segment window on disk (web player). hls_dir must already exist —
// ffmpeg's hls muxer does not create directories.
static void appendOutputArgs(std::vector<std::string>& a, const std::string& hls_dir)
{
	if (hls_dir.empty())
	{
		a.insert(a.end(), {
					 "-avoid_negative_ts", "make_zero",
					 "-muxdelay", "0", "-muxpreload", "0"
				 });
		a.insert(a.end(), {"-f", "mpegts", "pipe:1"});
		return;
	}
	// Per-branch muxer options must live inside that branch's [brackets] —
	// global options placed before -f tee do not propagate into sub-muxers.
	// muxdelay/muxpreload are rejected as unknown options inside a tee
	// bracket (verified against ffmpeg n8.1.1) even though they're valid
	// top-level mpegts options; avoid_negative_ts alone works.
	//
	// hls_time=2 (not the more typical 4-6s): the live encode is paced with
	// -re, so ffmpeg only closes/flushes segment 0 once hls_time seconds of
	// real wall-clock content has played — a first-tune-in cold start is
	// gated on that plus process spawn/codec-init overhead before any HLS
	// output exists at all. Measured ~4-5s with hls_time=4, which was
	// routinely losing the race against the client's manifest-load patience
	// (see Router.cpp's waitForFile). hls_time=2 roughly halves that floor.
	// omit_endlist: each scheduled item runs as its own ffmpeg process
	// (transition() spawns a new one per item), and the muxer would
	// otherwise write #EXT-X-ENDLIST when that process exits cleanly at the
	// item's end -- even though another process is about to append more
	// segments to this same playlist. hls.js treats ENDLIST as "stream over"
	// and once the client's buffer drains to it, calls
	// mediaSource.endOfStream(), which fires the <video> element's native
	// 'ended' event -- kicking the player out of the channel instead of
	// riding through the transition. The ChannelSession itself (not any one
	// ffmpeg process) owns when the stream actually ends.
	// temp_file: ffmpeg writes each segment and the playlist itself to a
	// "<name>.tmp" path and renames it into place only once the write is
	// complete, instead of rewriting <name> in place. Without this, a reader
	// (Router.cpp's serveHlsFile, or this same process's own
	// patchDiscontinuitySequence poll below) can land mid-write and see a
	// torn/garbled playlist.m3u8 — usually harmless (next poll/request
	// retries against a settled file), but at the one moment it actually
	// matters: append_list makes a brand-new transition's ffmpeg process
	// parse the *existing* playlist.m3u8 at startup to continue segment
	// numbering from where the previous process left off. A torn read there
	// makes that parse fail, so the new process silently restarts numbering
	// at seg-00000.ts — clobbering segment files a connected client's
	// already-fetched playlist still points at mid-fetch. That's the
	// mechanism behind "video/audio play the wrong content for several
	// seconds right after a channel transition" — rename-based writes make
	// every reader see either the fully-old or fully-new file, never a mix.
	std::string spec =
		"[f=mpegts:avoid_negative_ts=make_zero]pipe:1"
		"|[f=hls:hls_time=" + std::to_string(kLiveHlsSegmentSecs) +
		":hls_list_size=" + std::to_string(kLiveHlsListSize) +
		":hls_delete_threshold=" + std::to_string(kLiveHlsDeleteThreshold) +
		":hls_flags=delete_segments+append_list+omit_endlist+temp_file"
		":hls_segment_filename=" + hls_dir + "/seg-%05d.ts]" + hls_dir + "/playlist.m3u8";
	a.insert(a.end(), {"-f", "tee", spec});
}

static std::vector<std::string> buildArgs(
	const std::string& ffmpeg_path,
	const KairosNowResponse& item,
	int64_t startOffsetMs,
	int audioTrackIndex,
	int subtitleTrackIndex,
	bool loudnorm,
	HwAccel hw_accel,
	const std::string& vaapi_device,
	HwAccel decode_hw_accel,
	const std::set<std::string>& decodable_codecs,
	const std::string& source_codec,
	const VideoTrack* source_video,
	bool direct_stream,
	bool verbose_transcode_logs,
	double speed                      = 1.0,
	const std::string& max_resolution = "source",
	int video_bitrate_kbps            = 0,
	int audio_bitrate_kbps            = 192,
	const std::string& hls_dir        = "")
{
	std::vector<std::string> a;
	a.push_back(ffmpeg_path);
	pushLogLevelArgs(a, verbose_transcode_logs);

	// Reduce startup latency / buffer. +discardcorrupt deliberately dropped
	// from the original "+genpts+discardcorrupt" combo (a generic low-
	// latency-streaming recipe copied into the very first Hephaestus
	// scaffold commit, never actually justified for this specific use) —
	// it's a demuxer heuristic for discarding packets it judges corrupt,
	// appropriate for a genuinely lossy source (flaky network capture, RTSP
	// packet loss), not a well-formed local file. Real-world reports of
	// periodic (every few seconds) black-frame/stutter/resync glitches on
	// live channels match a false-positive drop of a legitimate-but-unusual
	// packet (VFR content, edit lists, telecine-era masters — real,
	// documented characteristics of this library elsewhere in this file)
	// far better than genuine corruption would explain.
	a.insert(a.end(), {"-fflags", "+genpts", "-flags", "low_delay"});

	// Pace input reads to the source's native frame rate. Without this ffmpeg
	// transcodes as fast as the CPU/GPU allows (often many times real-time),
	// exits almost immediately, and the session races through the entire
	// schedule instead of simulating a live broadcast.
	a.push_back("-re");

	// AMD VAAPI: expose the render node before -i so the encoder/decoder can find it
	pushVaapiDeviceArg(a, hw_accel, decode_hw_accel, vaapi_device);

	// Seek before input for fast seek
	if (startOffsetMs > 0)
	{
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(3) << (startOffsetMs / 1000.0);
		a.push_back("-ss");
		a.push_back(ss.str());
	}

	// Decode offload is pointless (and unsupported by ffmpeg) on a pure
	// stream copy — nothing here is ever decoded.
	if (!direct_stream) pushHwAccelDecodeArgs(a, decode_hw_accel, decodable_codecs, source_codec);

	a.push_back("-i");
	a.push_back(item.file_path);

	if (direct_stream)
	{
		// Native/copy bucket (ChannelSession::kNativeBucket) — video stays a
		// pure stream copy, the entire point of this bucket (avoid the
		// CPU/GPU-heavy video encode). Audio is NOT copied though: a copy has
		// no filter graph to run loudnorm through, so direct-stream viewers
		// got no volume normalization at all, and requiring the *viewer's*
		// declared capabilities to already cover whatever codec happened to
		// be in the source file (isChannelDirectStreamable's old audio check)
		// needlessly bounced viewers onto the far more expensive default
		// bucket over an audio-only mismatch. An audio-only encode is cheap
		// enough not to matter next to the video encode this bucket exists to
		// avoid. Subtitles still aren't carried through this branch (mirrors
		// VOD's own direct-stream path, VodSession.cpp): muxing or burning a
		// subtitle stream both require decoding video regardless, which would
		// defeat the point of this bucket the way an audio-only encode
		// doesn't.
		a.insert(a.end(), {
					 "-map", "0:v:0?",
					 "-map", "0:a:" + std::to_string(audioTrackIndex) + "?",
					 "-dn", "-map_chapters", "-1",
					 "-c:v", "copy"
				 });
		// resync_audio=true: this is exactly the copy-video/re-encode-audio
		// pairing pushAudioEncoderArgs' own comment on that flag describes —
		// see there for why only this bucket needs it.
		pushAudioEncoderArgs(a, loudnorm, speed, audio_bitrate_kbps,
							 /*client_caps=*/std::nullopt, /*source_audio=*/nullptr,
							 /*debug_showinfo=*/verbose_transcode_logs, /*resync_audio=*/true);
	}
	else
	{
		// Stream selection: first video (optional), selected audio track (optional)
		a.insert(a.end(), {
					 "-map", "0:v:0?",
					 "-map", "0:a:" + std::to_string(audioTrackIndex) + "?"
				 });
		if (subtitleTrackIndex >= 0) a.insert(a.end(), {"-map", "0:s:" + std::to_string(subtitleTrackIndex) + "?"});

		// No data streams, no chapter metadata in output
		a.insert(a.end(), {"-dn", "-map_chapters", "-1"});

		// Force constant output frame rate. Without this, ffmpeg's per-version/
		// per-input default fps_mode behavior governs how a source with
		// irregular/variable frame timestamps (common on older syndicated TV
		// masters — duplicate/dropped frames baked in from a telecine/pulldown-
		// era encode) gets handled, feeding directly into -re's real-time
		// pacing and -force_key_frames' segment-cutting decisions below —
		// uncorrected irregular timing there means uneven segment durations
		// against this channel's tight kLiveHlsSegmentSecs window, which shows
		// up to viewers as stutter/rebuffering. Only the default (transcode)
		// bucket needs this — a stream copy has no filter graph to feed it.
		a.insert(a.end(), {"-fps_mode", "cfr"});

		// -fps_mode cfr with no explicit -r leaves ffmpeg to *guess* a target
		// rate from the input's own timestamps — the exact timing this whole
		// comment block just said can't be trusted. A wrong guess (e.g.
		// rounding a 23.976fps source to 24, or landing on some other nearby
		// rate) means cfr's own duplicate/drop-frame conversion now has to
		// correct for a mismatch that doesn't really exist, at a beat
		// frequency determined by however far off the guess is — periodic,
		// content-independent stutter that would show up as a repeating
		// glitch every so often for the entire life of the stream, matching
		// real reports of exactly that on the transcode bucket. Pin it to the
		// source's own declared nominal rate (already probed) instead, so cfr
		// has a precise target instead of guessing one from the same
		// untrustworthy timing it exists to correct.
		if (source_video && !source_video->r_frame_rate.empty()) a.insert(a.end(), {"-r", source_video->r_frame_rate});

		// Video encoder
		std::vector<std::string> vfParts;
		// Scale down to the configured max resolution — never upscales.
		pushScaleFilter(vfParts, resolveMaxHeight(max_resolution));
		if (speed != 1.0) vfParts.push_back("setpts=PTS/" + fmtSpeed(speed));

		pushVideoEncoderArgs(a, vfParts, hw_accel, kLiveHlsSegmentSecs, source_video);
		// Last in the chain: reports pts_time right before the frame hits the
		// encoder, paired with pushAudioEncoderArgs' ashowinfo below — see
		// that flag's own comment. Diagnostic only; off outside
		// verbose_transcode_logs.
		if (verbose_transcode_logs) vfParts.push_back("showinfo");
		pushVideoFilterArgs(a, vfParts);
		// Bitrate cap: keeps CQ/VBR quality-based encoding but adds an upper
		// bound, preventing huge spikes on complex/high-res content — a real,
		// per-channel admin-configured value if one is set, otherwise a
		// generous resolution-sized default (see defaultBitrateCapKbps's own
		// comment) rather than leaving this genuinely uncapped. A -re-paced
		// live encode has no slack to absorb an unbounded spike the way
		// file-based encoding would; a forced/scene-cut I-frame with no
		// ceiling at all is a known live-streaming anti-pattern.
		int effective_bitrate_kbps = video_bitrate_kbps > 0
										 ? video_bitrate_kbps
										 : defaultBitrateCapKbps(effectiveOutputHeight(resolveMaxHeight(max_resolution), source_video));
		pushBitrateCapArgs(a, effective_bitrate_kbps);

		// Audio: AAC
		pushAudioEncoderArgs(a, loudnorm, speed, audio_bitrate_kbps,
							 /*client_caps=*/std::nullopt, /*source_audio=*/nullptr,
							 /*debug_showinfo=*/verbose_transcode_logs);
	}

	appendOutputArgs(a, hls_dir);
	return a;
}

// Loops a still image into an MPEG-TS stream, with either a looped audio
// track or generated silence. Used for the offline slate and the connect-time
// splash — neither has a real media file to seek/map into like buildArgs().
// max_duration_ms > 0 bounds the encode so it naturally exits and lets
// onExit()/transition() re-poll Kairos afterward, instead of looping the
// slate forever with no way to notice real programming has resumed — see
// spawnOffline's comment for when each mode applies.
static std::vector<std::string> buildImageArgs(
	const std::string& ffmpeg_path,
	const std::string& image_path,
	const std::string& audio_path, // empty = generate silence
	HwAccel hw_accel,
	const std::string& vaapi_device,
	const std::string& max_resolution,
	int video_bitrate_kbps,
	int audio_bitrate_kbps,
	bool verbose_transcode_logs,
	const std::string& hls_dir = "",
	int64_t max_duration_ms    = 0)
{
	std::vector<std::string> a;
	a.push_back(ffmpeg_path);
	pushLogLevelArgs(a, verbose_transcode_logs);

	a.insert(a.end(), {"-fflags", "+genpts", "-flags", "low_delay"});
	a.push_back("-re");

	// Image loop input never decodes a real file, so only encode-side amd
	// (not decode) can ever need the device here.
	pushVaapiDeviceArg(a, hw_accel, HwAccel::none, vaapi_device);

	// Input 0: the image, looped forever. A still image has no frame rate of
	// its own — pick a normal one so players see a standard CFR stream.
	// -f image2 must be forced explicitly: image_path can be a remote URL
	// (offline_image_path/logo_path both accept "path or URL"), and ffmpeg's
	// format auto-detection picks a pipe-style demuxer (e.g. png_pipe) for
	// those instead of image2 — -loop is an image2-demuxer option, so on a
	// pipe demuxer it's silently ignored on the first pass and then corrupts
	// the stream on the second read attempt (the underlying avio can't
	// properly re-fetch a non-seekable network resource from byte 0).
	a.insert(a.end(), {"-f", "image2", "-loop", "1", "-framerate", "25", "-i", image_path});

	// Input 1: configured offline audio (looped) or generated silence.
	if (!audio_path.empty()) a.insert(a.end(), {"-stream_loop", "-1", "-i", audio_path});
	else a.insert(a.end(), {"-f", "lavfi", "-i", "anullsrc=channel_layout=stereo:sample_rate=48000"});

	a.insert(a.end(), {"-map", "0:v:0", "-map", "1:a:0"});
	a.insert(a.end(), {"-dn", "-map_chapters", "-1"});

	std::vector<std::string> vfParts;
	pushScaleFilter(vfParts, resolveMaxHeight(max_resolution));

	pushVideoEncoderArgs(a, vfParts, hw_accel, kLiveHlsSegmentSecs);
	pushVideoFilterArgs(a, vfParts);
	// Same "don't leave this genuinely uncapped" reasoning as the real-content
	// branch above, even though a static image's own real bitrate need is
	// negligible regardless — consistency, not because this path is expected
	// to ever actually hit the cap.
	int effective_bitrate_kbps = video_bitrate_kbps > 0
									 ? video_bitrate_kbps
									 : defaultBitrateCapKbps(effectiveOutputHeight(resolveMaxHeight(max_resolution), nullptr));
	pushBitrateCapArgs(a, effective_bitrate_kbps);

	pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, audio_bitrate_kbps);

	if (max_duration_ms > 0)
	{
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(3) << (max_duration_ms / 1000.0);
		a.push_back("-t");
		a.push_back(ss.str());
	}

	appendOutputArgs(a, hls_dir);
	return a;
}

// ── ChannelSession ────────────────────────────────────────────────────────────

ChannelSession::ChannelSession(std::string channel_id, KairosClient& kairos,
							   std::string ffmpeg_path, StreamOptions opts, std::string bucket)
	: channel_id(std::move(channel_id))
	, kairos(kairos)
	, ffmpeg_path(std::move(ffmpeg_path))
	, opts(std::move(opts))
	, bucket(std::move(bucket))
{
}

ChannelSession::~ChannelSession()
{
	stop();
	hls_watcher_stop.store(true);
	if (hls_watcher.joinable()) hls_watcher.join();
	hls_patch_stop.store(true);
	if (hls_patch_thread.joinable()) hls_patch_thread.join();
	prefetch_stop.store(true);
	if (prefetch_thread.joinable()) prefetch_thread.join();
}

std::string ChannelSession::hlsDir() const
{
	if (opts.hls_root.empty()) return "";
	return opts.hls_root + "/live/" + channel_id + "/" + bucket;
}

void ChannelSession::touchHls()
{
	last_hls_touch_ms.store(nowMs());
}

// True when HLS is disabled, or has never been touched, or hasn't been
// touched within the linger window — i.e. it's safe to stop on HLS grounds.
bool ChannelSession::hlsIdle() const
{
	if (opts.hls_root.empty()) return true;
	int64_t touch = last_hls_touch_ms.load();
	if (touch == 0) return true;
	return (nowMs() - touch) > static_cast<int64_t>(opts.linger_secs) * 1000;
}

// Polls because HLS itself is poll-based — there's no persistent connection
// to react to the way removeClient() reacts to an MPEG-TS client dropping.
// Without this, a channel watched only via HLS (zero MPEG-TS ClientSinks)
// would never trigger scheduleStop() at all and run forever.
void ChannelSession::hlsWatchLoop()
{
	while (!hls_watcher_stop.load() && active.load())
	{
		std::this_thread::sleep_for(std::chrono::seconds(5));
		if (!active.load()) return;
		if (client_count.load() == 0 && hlsIdle())
		{
			stop();
			return;
		}
	}
}

// Ticks faster than hlsWatchLoop (segments are only kLiveHlsSegmentSecs
// long, so a stale discontinuity-sequence needs correcting well within one
// segment interval, not one linger-check interval) — kept as its own
// thread/loop rather than folded into hlsWatchLoop so shortening this one's
// cadence doesn't also change the idle/linger check's timing.
void ChannelSession::hlsPatchLoop()
{
	while (!hls_patch_stop.load() && active.load())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(kLiveHlsSegmentSecs * 500));
		if (!active.load()) return;
		patchDiscontinuitySequence();
	}
}

void ChannelSession::patchDiscontinuitySequence()
{
	std::string path = hlsDir() + "/playlist.m3u8";

	std::ifstream in(path);
	if (!in) return;
	std::vector<std::string> lines;
	std::string line;
	int visible = 0;
	while (std::getline(in, line))
	{
		if (line == "#EXT-X-DISCONTINUITY") ++visible;
		lines.push_back(line);
	}
	in.close();

	// Segment-duration sanity check, piggybacked on the read above (no extra
	// I/O): flags a newly-appeared segment whose actual muxed EXTINF duration
	// deviates meaningfully from the requested hls_time (kLiveHlsSegmentSecs)
	// — evidence the HLS muxer itself, not per-frame encode timing (already
	// ruled clean by the showinfo/ashowinfo filters), is cutting segments
	// irregularly, e.g. off-GOP-boundary at short segment durations. Only
	// checks the newest segment each tick (cheap, and sufficient to catch a
	// pattern over time); diagnostic only, gated on verbose_transcode_logs
	// like showinfo/ashowinfo.
	if (opts.verbose_transcode_logs)
	{
		int lastExtinfIdx = -1;
		for (size_t i = 0; i < lines.size(); ++i) if (lines[i].rfind("#EXTINF:", 0) == 0) lastExtinfIdx = static_cast<int>(i);
		if (lastExtinfIdx >= 0 && static_cast<size_t>(lastExtinfIdx) + 1 < lines.size())
		{
			const std::string& uri = lines[static_cast<size_t>(lastExtinfIdx) + 1];
			if (uri != last_extinf_uri_)
			{
				last_extinf_uri_ = uri;
				double dur       = 0.0;
				try { dur = std::stod(lines[static_cast<size_t>(lastExtinfIdx)].substr(8)); }
				catch (...)
				{
				}
				double expected = static_cast<double>(kLiveHlsSegmentSecs);
				if (dur > 0.0 && std::fabs(dur - expected) > expected * 0.2)
				{
					std::cerr << "[session:" << channel_id << "] segment " << uri
						<< " duration=" << dur << "s deviates from hls_time=" << expected << "s\n";
				}
			}
		}
	}

	// ffmpeg's own writes to this file are rename-based now (hls_flags
	// +temp_file, see appendOutputArgs), so a torn read here would only ever
	// come from this loop racing itself across ticks — shouldn't happen
	// given the single-threaded poll below, but a missing/garbled header is
	// still the unambiguous sign something upstream (e.g. mid-restart with
	// the dir freshly created) isn't settled yet; skip and let the next
	// tick (half a segment interval away) retry.
	if (lines.empty() || lines[0] != "#EXTM3U") return;

	int wanted = discontinuity_count_.load() - visible;
	if (wanted < 0) wanted = 0; // defensive only — should never actually go negative

	std::string wantedLine = "#EXT-X-DISCONTINUITY-SEQUENCE:" + std::to_string(wanted);

	int existingIdx = -1;
	for (size_t i = 0; i < lines.size(); ++i)
	{
		if (lines[i].rfind("#EXT-X-DISCONTINUITY-SEQUENCE:", 0) == 0)
		{
			existingIdx = static_cast<int>(i);
			break;
		}
	}
	if (existingIdx >= 0 && lines[static_cast<size_t>(existingIdx)] == wantedLine) return; // already correct

	if (existingIdx >= 0)
	{
		lines[static_cast<size_t>(existingIdx)] = wantedLine;
	}
	else
	{
		// Standard placement: right after #EXT-X-VERSION (falls back to
		// right after #EXTM3U if that's somehow absent), before any Media
		// Segment — same neighborhood ffmpeg already puts #EXT-X-TARGETDURATION.
		size_t insertAt = 1;
		for (size_t i = 0; i < lines.size(); ++i)
		{
			if (lines[i].rfind("#EXT-X-VERSION", 0) == 0)
			{
				insertAt = i + 1;
				break;
			}
		}
		lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), wantedLine);
	}

	// Rename-based write, same reasoning as ffmpeg's own +temp_file above:
	// this file is read by Router.cpp's serveHlsFile and re-parsed at
	// startup by every transition's new ffmpeg process (hls_flags=
	// append_list), both of which must never observe a half-written file.
	std::string tmpPath = path + ".tmp";
	{
		std::ofstream out(tmpPath, std::ios::trunc);
		if (!out) return;
		for (auto& l : lines) out << l << "\n";
	}
	std::error_code ec;
	std::filesystem::rename(tmpPath, path, ec);
}

// Looks ahead to whatever Kairos has scheduled after the current item and
// warms probeMediaCached()'s process-lifetime cache for it in the
// background, so spawnFfmpeg()'s own probeMediaCached() call at the actual
// transition is a cache hit instead of a cold ffprobe on the hot path — the
// same class of fix as snapToKeyframe()/keyframes_ms, just for the audio/
// subtitle/codec probe rather than keyframe timestamps (Kairos doesn't
// sync-cache that half, so this warms it live instead of relying on sync).
// current_item is read under current_item_mtx (see its own comment) — a
// stale value here (the scheduling thread reassigning it moments after this
// snapshot) is still fine, same tolerance as currentTitle()/currentFilePath():
// worst case this tick prefetches the wrong thing and the next one corrects
// it, no correctness impact since spawnFfmpeg() always re-probes from
// item.file_path itself regardless. Only the read itself needed to stop
// being racy.
void ChannelSession::prefetchLoop()
{
	while (!prefetch_stop.load() && active.load())
	{
		std::this_thread::sleep_for(std::chrono::seconds(kPrefetchTickSecs));
		if (!active.load()) return;

		int64_t end_ms;
		std::string cur;
		{
			std::lock_guard<std::mutex> l(current_item_mtx);
			end_ms = current_item.wall_clock_end_ms;
			cur    = current_item.file_path;
		}
		if (end_ms <= 0) continue;
		if (nowMs() < end_ms - kPrefetchLeadMs) continue;
		if (prefetched_for_end_ms.exchange(end_ms) == end_ms) continue; // already warmed this cycle

		auto next = kairos.getNext(channel_id);
		if (!next || next->file_path.empty() || next->file_path == cur) continue;

		std::string path = next->file_path, ffprobe = opts.ffprobe_path;
		TaskRegistry::global().spawn([path, ffprobe] { probeMediaCached(ffprobe, path); });
	}
}

// Computes how far into `item` playback should start, given true wall-clock
// time `atMs`. Fillers loop on their own duration; non-fillers clamp to 0.
int64_t ChannelSession::computeOffset(const KairosNowResponse& item, int64_t atMs)
{
	int64_t rawOffset = std::max(int64_t(0), atMs - item.wall_clock_start_ms);
	return (item.is_filler && item.duration_ms > 0)
			   ? rawOffset % item.duration_ms
			   : rawOffset;
}

int64_t ChannelSession::snapToKeyframe(const KairosNowResponse& item, int64_t offsetMs)
{
	if (offsetMs <= 0 || item.keyframes_ms.empty() || item.file_path.empty()) return offsetMs;

	std::error_code ec;
	const auto file_size = std::filesystem::file_size(item.file_path, ec);
	const bool cache_hit = !ec
		&& item.keyframes_size == static_cast<int64_t>(file_size)
		&& item.keyframes_mtime == statMtimeEpochSecs(item.file_path);
	if (!cache_hit) return offsetMs;

	// Largest keyframe timestamp <= offsetMs — never snap forward past the
	// intended start point. keyframes_ms is sorted ascending (Kairos's own
	// packet-level scan reads them in stream order).
	auto it = std::upper_bound(item.keyframes_ms.begin(), item.keyframes_ms.end(), offsetMs);
	if (it == item.keyframes_ms.begin()) return offsetMs; // no keyframe at/before offset
	return *(--it);
}

std::optional<double> ChannelSession::computeSpeed(int64_t rawDriftMs, int64_t durationMs)
{
	if (durationMs <= 0 || rawDriftMs == 0) return std::nullopt;

	// Playing at duration / (duration - drift) means the item takes exactly
	// (duration - drift) ms of wall-clock time to play `duration` ms of
	// content — i.e. the gap is fully closed by the time it ends. Positive
	// drift (running behind schedule) yields speed > 1 (speed up); negative
	// drift (running ahead) yields speed < 1 (slow down).
	double speed = static_cast<double>(durationMs) /
		static_cast<double>(durationMs - rawDriftMs);

	// Only apply the correction if it's small enough to stay within our
	// near-imperceptible clamp unclamped — clamping a correction that needed
	// to be much larger would apply an audible speed change while still
	// failing to close the gap, which is strictly worse than just seeking.
	if (speed < kMinSpeed || speed > kMaxSpeed) return std::nullopt;

	return speed;
}

bool ChannelSession::start()
{
	active            = true;
	in_splash         = false;
	session_start_ms  = nowMs();
	last_hls_touch_ms = session_start_ms;

	auto dir = hlsDir();
	if (!dir.empty())
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (ec)
		{
			std::cerr << "[session:" << channel_id << "] failed to create HLS dir \""
				<< dir << "\": " << ec.message() << " — HLS output disabled for this session\n";
			opts.hls_root.clear();
		}
		else
		{
			hls_watcher      = std::thread([this] { hlsWatchLoop(); });
			hls_patch_thread = std::thread([this] { hlsPatchLoop(); });
		}
	}

	// Unlike hls_watcher/hls_patch_thread above, this isn't HLS-specific —
	// MPEG-TS-only sessions get transitions too — so it always starts.
	prefetch_thread = std::thread([this] { prefetchLoop(); });

	// Kick off the /now lookup on its own thread and give it a short budget
	// to answer before deciding whether to show the splash at all.
	int64_t at                                        = nowMs();
	auto prom                                         = std::make_shared<std::promise<std::optional<KairosNowResponse>>>();
	std::future<std::optional<KairosNowResponse>> fut = prom->get_future();
	TaskRegistry::global().spawn([self = shared_from_this(), prom, at]
	{
		prom->set_value(self->kairos.getNow(self->channel_id, at));
	});

	if (fut.wait_for(kFastPathBudget) == std::future_status::ready)
	{
		// Fast path (the common case): apply the already-resolved item directly,
		// never touching the splash.
		applyResolvedItem(fut.get(), at);
	}
	else
	{
		// Kairos hasn't answered within budget (cold start / network blip) — show
		// the splash now so the client gets bytes immediately, and swap over once
		// the lookup (still in flight on its own thread) finally completes.
		in_splash = true;
		spawnOffline(KairosNowResponse{});
		TaskRegistry::global().spawn([self = shared_from_this(), fut = std::move(fut), at]() mutable
		{
			self->applyResolvedItem(fut.get(), at);
		});
	}

	return active.load();
}

void ChannelSession::applyResolvedItem(std::optional<KairosNowResponse> item, int64_t at)
{
	if (!item)
	{
		// Same bounded-retry shape transition()'s own "Kairos has nothing at
		// all" fallback uses (see its comment there) — deliberately NOT the
		// unbounded cold-start splash (KairosNowResponse{}, wall_clock_end_ms
		// == 0, loops forever) start()'s own kFastPathBudget branch spawns:
		// that one only ever gets replaced by a second, independent /now
		// resolution, and there isn't one here — this call *is* the
		// resolution, and it just failed. Without this, a session whose
		// very first /now lookup fails (Kairos down/restarting right as a
		// viewer connects) got stuck in_splash forever, with nothing left to
		// ever retry — current_item/in_splash=false are what let onExit()
		// naturally chain into transition() (and so back into this same
		// retry loop) once this bounded encode finishes, instead of landing
		// in onExit()'s in_splash branch, which just respawns the same
		// unbounded splash forever.
		std::cerr << "[session:" << channel_id << "] /now failed at startup, showing default slate and retrying\n";
		KairosNowResponse fallback;
		fallback.item_type           = "offline";
		fallback.wall_clock_start_ms = at;
		fallback.wall_clock_end_ms   = at + kNoContentRetryMs;
		{
			std::lock_guard<std::mutex> l(current_item_mtx);
			current_item = fallback;
		}
		item_start             = steady_::now();
		current_item_offset_ms = 0;
		in_splash              = false;
		spawnFfmpeg(fallback, 0);
		return;
	}

	// Compute how far into the item we are
	int64_t startOffset = computeOffset(*item, at);

	{
		std::lock_guard<std::mutex> l(current_item_mtx);
		current_item = *item;
	}
	item_start             = steady_::now();
	current_item_offset_ms = startOffset;
	in_splash              = false;

	// Prefer a gentle speed correction (whole item plays start-to-finish,
	// just slightly faster/slower) over seeking into content when the drift
	// is small enough to close that way. Gated on opts.force_transcode, NOT
	// just "not the native bucket": a dual-bucket channel (force_transcode
	// false) can have both a default- and native-bucket viewer watching the
	// "same" channel at once, and native can never speed-correct (a stream
	// copy has no filter graph) — speeding up only the default bucket would
	// let those two viewers silently drift apart in actual content position,
	// defeating the point of it being the same live channel. Only safe to
	// speed-correct when this channel has no other bucket to stay in sync
	// with, i.e. direct-stream is disabled for it entirely. (bucket !=
	// kNativeBucket is also checked as a defensive belt-and-suspenders — see
	// spawnFfmpeg's own comment — since a native-bucket session should never
	// have force_transcode=true in the first place: ChannelViewerRegistry
	// never resolves or spawns native for such a channel.)
	double speed = 1.0;
	if (opts.force_transcode && bucket != kNativeBucket && !item->is_filler && item->duration_ms > 0)
	{
		int64_t rawDrift = at - item->wall_clock_start_ms;
		if (auto s = computeSpeed(rawDrift, item->duration_ms))
		{
			speed                  = *s;
			startOffset            = 0;
			current_item_offset_ms = 0;
		}
	}

	// If the offset meets or exceeds the item's known duration, ffmpeg would
	// seek past EOF and exit immediately with code=0, looping indefinitely.
	// Skip to transition directly instead.
	if (!item->is_filler && item->duration_ms > 0 && startOffset >= item->duration_ms)
	{
		std::cerr << "[session:" << channel_id << "] startup offset " << startOffset
			<< "ms >= duration " << item->duration_ms << "ms for \""
			<< item->file_path << "\", skipping to next item\n";
		transition(); // already off the connect thread, no need to detach again
		return;
	}

	spawnFfmpeg(*item, startOffset, speed);
}

void ChannelSession::stop()
{
	if (!active.exchange(false)) return;
	{
		std::lock_guard<std::mutex> lock(ffmpeg_mtx);
		if (ffmpeg)
		{
			ffmpeg->kill();
			ffmpeg.reset();
		}
	}
	broadcastDone();
	auto dir = hlsDir();
	if (!dir.empty())
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}
}

void ChannelSession::addClient(std::shared_ptr<ClientSink> sink)
{
	std::lock_guard<std::mutex> lock(clients_mtx);
	clients.push_back(sink);
	++client_count;
}

void ChannelSession::removeClient(std::shared_ptr<ClientSink> sink)
{
	{
		std::lock_guard<std::mutex> lock(clients_mtx);
		clients.erase(std::remove(clients.begin(), clients.end(), sink), clients.end());
		--client_count;
	}
	if (client_count.load() == 0) scheduleStop();
}

void ChannelSession::onData(const uint8_t* data, size_t len)
{
	std::lock_guard<std::mutex> lock(clients_mtx);
	std::vector<uint8_t> chunk(data, data + len);
	for (auto& sink : clients)
	{
		std::lock_guard<std::mutex> sl(sink->mtx);
		if (sink->queue.size() >= ClientSink::MAX_QUEUE)
		{
			// Client can't keep up — drop oldest chunk rather than blocking
			sink->queue.pop_front();
		}
		sink->queue.push_back(chunk);
		sink->cv.notify_one();
	}
}

void ChannelSession::onExit(int code)
{
	if (!active.load()) return; // we were stopped intentionally

	std::cerr << "[session:" << channel_id << "] ffmpeg exited (code=" << code << ")\n";

	if (in_splash.load())
	{
		// The splash loops forever and should never exit on its own; if it
		// does, just respawn it — the background /now lookup from start() is
		// still running independently and will swap over once it's ready.
		TaskRegistry::global().spawn([self = shared_from_this()] { self->spawnOffline(KairosNowResponse{}); });
		return;
	}

	// Natural exit (code 0): item finished cleanly → transition.
	// Unexpected exit (code != 0): also transition to avoid black-screen.
	TaskRegistry::global().spawn([self = shared_from_this()] { self->transition(); });
}

void ChannelSession::transition()
{
	if (!active.load()) return;

	int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		steady_::now() - item_start).count();

	// Snapshot once under lock (see current_item_mtx's own comment) instead
	// of re-reading the live member throughout this function — transition()
	// only ever runs after the ffmpeg process for this exact item has
	// already exited, so nothing else legitimately reassigns current_item
	// out from under this snapshot before the writes further down; this
	// just closes the torn-read window against prefetchLoop()/
	// currentTitle()/currentFilePath() on other threads.
	KairosNowResponse cur;
	{
		std::lock_guard<std::mutex> l(current_item_mtx);
		cur = current_item;
	}

	// Offline gap-fillers carry no item_id (nothing scheduled to mark played)
	// — Kairos's /played rejects an empty one outright, so skip the call
	// rather than firing a request guaranteed to log a spurious error on
	// every bounded-offline recheck (see spawnOffline).
	if (cur.item_type != "offline") kairos.markPlayed(channel_id, cur.item_type, cur.item_id, cur.block_id, elapsed);

	// The item's *actual* real-time playback duration (elapsed) can drift
	// from its scheduled duration_ms — rounding, edit lists, VFR content, or
	// ffprobe estimation error at scheduling time. That drift is proportional
	// to item length, so it's invisible on short fillers but can be several
	// seconds off after a long movie/episode. Anchor "true now" on the actual
	// elapsed play time (including whatever offset the current item itself
	// started at, so drift keeps accumulating correctly across repeated
	// transitions instead of resetting each time) rather than trusting
	// wall_clock_end_ms verbatim.
	int64_t actualNowMs = cur.wall_clock_start_ms + current_item_offset_ms + elapsed;

	// Look up the next item using the *scheduled* end time (not actualNowMs)
	// so we request it deterministically and never skip ahead into real
	// programming due to drift — only the resulting offset is drift-corrected.
	auto next = kairos.getNow(channel_id, cur.wall_clock_end_ms);
	if (!next)
	{
		// Fallback: try current wall clock
		next = kairos.getNow(channel_id, nowMs());
	}
	if (!next)
	{
		// Kairos has nothing at all for this channel right now — no
		// scheduled item, no filler, no offline config (a channel-level
		// /now failure, distinct from spawnFfmpeg's item_type=="offline"
		// path below, which at least got a real response). This used to
		// kill the whole session outright on any such gap, requiring a full
		// restart to recover once Kairos had something to offer again.
		// Fall back to Hephaestus's own bundled default slate instead (same
		// last-resort default_logo_path spawnOffline already falls back to)
		// and keep retrying on a bounded recheck.
		std::cerr << "[session:" << channel_id << "] cannot get next item, showing default slate and retrying\n";
		KairosNowResponse fallback;
		fallback.item_type           = "offline";
		fallback.wall_clock_start_ms = actualNowMs;
		fallback.wall_clock_end_ms   = actualNowMs + kNoContentRetryMs;
		{
			std::lock_guard<std::mutex> l(current_item_mtx);
			current_item = fallback;
		}
		item_start             = steady_::now();
		current_item_offset_ms = 0;
		spawnFfmpeg(fallback, 0);
		return;
	}

	// If we've fallen far enough behind that we've already blown past an
	// entire filler's scheduled window, skip it outright instead of starting
	// partway into a "random" repeat of it (fillers are low-stakes/
	// interchangeable padding — unlike real programming, dropping one
	// entirely is the better trade for clawing back accumulated drift).
	// Never skips a non-filler item this way — only ever advances past
	// fillers whose entire window has already elapsed.
	int guard = 0;
	while (next->is_filler && next->duration_ms > 0 &&
		actualNowMs >= next->wall_clock_end_ms && ++guard < 64)
	{
		auto after = kairos.getNow(channel_id, next->wall_clock_end_ms);
		if (!after) break;
		next = after;
	}

	int64_t startOffset = computeOffset(*next, actualNowMs);

	{
		std::lock_guard<std::mutex> l(current_item_mtx);
		current_item = *next;
	}
	item_start             = steady_::now();
	current_item_offset_ms = startOffset;

	// Prefer a gentle speed correction over seeking into content when the
	// drift is small enough to close over the course of the whole item —
	// avoids ever cutting off the beginning of real programming just to
	// stay synced. Fillers are excluded: their own loop-offset/skip logic
	// already absorbs drift, and their short duration makes any speed nudge
	// both less effective and more perceptible. Gated on opts.force_transcode
	// (not just "not the native bucket") — see applyResolvedItem()'s matching
	// comment: a dual-bucket channel's default and native buckets must stay
	// positionally identical, since native can never speed-correct at all —
	// otherwise two viewers of the "same" channel could drift apart in actual
	// content position. Only safe once this channel has no native bucket to
	// stay in sync with.
	double speed = 1.0;
	if (opts.force_transcode && bucket != kNativeBucket && !next->is_filler && next->duration_ms > 0)
	{
		int64_t rawDrift = actualNowMs - next->wall_clock_start_ms;
		if (auto s = computeSpeed(rawDrift, next->duration_ms))
		{
			speed                  = *s;
			startOffset            = 0;
			current_item_offset_ms = 0;
		}
	}

	// Same EOF-seek guard as start(): if drift pushed us past this item's
	// duration too, skip straight to the one after it instead of spawning
	// ffmpeg with an offset past EOF.
	if (!next->is_filler && next->duration_ms > 0 && startOffset >= next->duration_ms)
	{
		std::cerr << "[session:" << channel_id << "] transition offset " << startOffset
			<< "ms >= duration " << next->duration_ms << "ms for \""
			<< next->file_path << "\", skipping to next item\n";
		TaskRegistry::global().spawn([self = shared_from_this()] { self->transition(); });
		return;
	}

	// Every transition spawns a brand new ffmpeg process into the same
	// output file (see launchFfmpeg's swap-under-mutex tail) — ffmpeg's own
	// HLS muxer marks that restart with #EXT-X-DISCONTINUITY, so this needs
	// to count in step with it for patchDiscontinuitySequence() below.
	discontinuity_count_.fetch_add(1);
	spawnFfmpeg(*next, startOffset, speed);
}

void ChannelSession::spawnFfmpeg(const KairosNowResponse& item, int64_t startOffsetMs, double speed)
{
	if (item.item_type == "offline")
	{
		spawnOffline(item);
		return;
	}

	if (item.file_path.empty())
	{
		std::cerr << "[session:" << channel_id << "] item has no file_path, skipping\n";
		active = false;
		broadcastDone();
		return;
	}

	bool direct_stream = (bucket == kNativeBucket);
	// A stream copy can't apply the setpts drift-correction filter
	// transition()/applyResolvedItem() may compute — those callers now skip
	// that decision entirely for this bucket (see their own comments) so
	// startOffsetMs already reflects the real, correct wall-clock catch-up
	// offset rather than one zeroed out by a since-discarded speed choice.
	// This is just the defensive backstop in case a caller is ever added
	// that doesn't know to skip it.
	if (direct_stream) speed = 1.0;

	// Pre-snap to a real keyframe using Kairos's sync-time probe, instead of
	// leaving that same landing point for ffmpeg's own -ss seek-and-search to
	// discover fresh on every single transition — see snapToKeyframe's own
	// comment.
	if (direct_stream) startOffsetMs = snapToKeyframe(item, startOffsetMs);

	std::cout << "[session:" << channel_id << ":" << bucket << "] spawning ffmpeg: \""
		<< item.file_path << "\" offset=" << startOffsetMs << "ms";
	if (speed != 1.0) std::cout << " speed=" << speed;
	std::cout << "\n";

	// Audio + subtitle track selection via ffprobe. Always probed now (not
	// just when hwaccel/lang settings are in play) — correct HDR handling
	// needs this item's source color info regardless of those other
	// settings, and probeMediaCached makes every probe past the first one
	// for a given file free.
	int audioTrack    = 0;
	int subtitleTrack = -1;
	std::string source_codec;
	auto info = probeMediaCached(opts.ffprobe_path, item.file_path);
	if (info)
	{
		if (!opts.audio_lang.empty()) audioTrack = pickAudioTrack(*info, opts.audio_lang);
		if (!opts.subtitle_lang.empty()) subtitleTrack = pickSubtitleTrack(*info, opts.subtitle_lang);
		if (!info->video.empty()) source_codec = decodeCodecKey(info->video[0].codec, info->video[0].bit_depth);
	}
	const VideoTrack* source_video = (info && !info->video.empty()) ? &info->video[0] : nullptr;

	{
		std::lock_guard<std::mutex> lock(info_mtx);
		current_info        = info;
		current_audio_track = audioTrack;
	}
	// Notify a wired-up ChannelViewerRegistry (see onItemPlaying's own
	// comment) what's now playing, so it can refresh capability-bucketed
	// viewers' recommended bucket. Harmless if more than one bucket instance
	// calls this for the same item — idempotent recomputation against the
	// same deterministic Kairos schedule.
	if (onItemPlaying) onItemPlaying(channel_id, info, audioTrack, item.duration_ms, item.is_filler);

	auto args = buildArgs(ffmpeg_path, item, startOffsetMs, audioTrack, subtitleTrack,
						  opts.loudnorm, opts.hw_accel, opts.vaapi_device,
						  opts.decode_hw_accel, opts.decodable_codecs, source_codec, source_video,
						  direct_stream,
						  opts.verbose_transcode_logs, speed,
						  opts.max_resolution, opts.video_bitrate_kbps, opts.audio_bitrate_kbps,
						  hlsDir());

	// Re-anchor item_start here — right before the real ffmpeg process
	// actually launches — overriding the caller's own (necessarily earlier)
	// assignment. probeMediaCached() above is a cache hit almost always
	// (prefetchLoop() warms it ahead of time), but on a miss (slow network
	// share, prefetch lead too short) it's a real synchronous ffprobe call
	// that can take real wall-clock time. transition()'s
	// elapsed = steady_::now() - item_start feeds directly into
	// actualNowMs's drift calculation, which decides whether the *next* item
	// gets a gentle speed nudge or a hard seek into its content — counting
	// probe latency as if it were real paced playback inflates that drift
	// with time ffmpeg was never actually running, and can push an otherwise
	// on-time transition over computeSpeed's ±2% bound, seeking into (and
	// skipping the true beginning of) the next item for no real scheduling
	// reason. NOTE: don't remove the caller's own earlier item_start
	// assignment (applyResolvedItem()/transition()) as a "redundant write" —
	// it's still load-bearing for the EOF-seek-guard path just above each
	// call site, which calls transition() directly instead of this function
	// and never reaches this line, so it needs its own fresh (near-zero
	// elapsed, since nothing was ever actually played) anchor.
	item_start = steady_::now();
	launchFfmpeg(std::move(args), "ffmpeg");
}

// Resolves an image to loop for an "offline" item (dead-air slate or connect
// splash): the item's own offline_image_path, else this channel's logo, else
// the bundled generic default. Only bails with no stream if all three are
// unset, which shouldn't happen once default_logo_path is configured.
//
// item.wall_clock_end_ms distinguishes the two callers: the cold-start/
// in_splash connect splash always passes a default-constructed
// KairosNowResponse{} (wall_clock_end_ms == 0) and should loop forever —
// the in-flight /now lookup that's actually resolving it runs independently
// and swaps in real content the moment it's ready (see start()/onExit()).
// A genuine scheduled gap reached through transition() carries a real
// wall_clock_end_ms, and *must* be bounded: buildImageArgs' -loop/
// -stream_loop otherwise runs the encode forever with nothing to ever
// notice the gap has ended and real programming has resumed, permanently
// stranding the session (the exact bug this bound exists to fix).
void ChannelSession::spawnOffline(const KairosNowResponse& item)
{
	std::string image = item.offline_image_path.value_or("");
	if (image.empty()) image = opts.logo_path;
	if (image.empty()) image = opts.default_logo_path;

	if (image.empty())
	{
		std::cerr << "[session:" << channel_id << "] no offline image available, skipping\n";
		active = false;
		broadcastDone();
		return;
	}

	int64_t boundMs = 0;
	if (item.wall_clock_end_ms > 0)
	{
		// Clamped to a sane minimum: wall_clock_end_ms can already be at or
		// slightly behind "now" by the time this runs (request latency,
		// clock drift) — a near-zero -t would spin the recheck loop far
		// faster than intended.
		static constexpr int64_t kMinRecheckMs = 3000;
		boundMs                                = std::max(kMinRecheckMs, item.wall_clock_end_ms - nowMs());
	}

	std::cout << "[session:" << channel_id << "] spawning ffmpeg (offline slate): \"" << image << "\"";
	if (boundMs > 0) std::cout << " recheck in " << boundMs << "ms";
	std::cout << "\n";

	auto args = buildImageArgs(ffmpeg_path, image, item.offline_audio_path.value_or(""),
							   opts.hw_accel, opts.vaapi_device, opts.max_resolution,
							   opts.video_bitrate_kbps, opts.audio_bitrate_kbps,
							   opts.verbose_transcode_logs, hlsDir(), boundMs);
	launchFfmpeg(std::move(args), "ffmpeg (offline slate)");
}

// Shared tail of spawnFfmpeg()/spawnOffline(): installs `args` as the
// session's ffmpeg process, replacing (and killing, via unique_ptr swap) any
// prior one — this is the same mechanism transition() uses between scheduled
// items, so it's also what a splash-to-real-content swap uses.
void ChannelSession::launchFfmpeg(std::vector<std::string> args, const char* what)
{
	std::lock_guard<std::mutex> lock(ffmpeg_mtx);
	// Re-check active after acquiring the mutex: stop() may have run between
	// building args (which can involve an ffprobe call) and here, leaving
	// active=false and ffmpeg=nullptr.
	if (!active.load()) return;

	ffmpeg = std::make_unique<FfmpegProcess>(
		std::move(args),
		[this](const uint8_t* d, size_t l) { onData(d, l); },
		[this](int code) { onExit(code); },
		opts.buffer_size,
		opts.ffmpeg_debug_logs,
		opts.verbose_transcode_logs
	);

	if (!ffmpeg->start())
	{
		std::cerr << "[session:" << channel_id << "] failed to spawn " << what << "\n";
		active = false;
		broadcastDone();
	}
}

void ChannelSession::broadcastDone()
{
	std::lock_guard<std::mutex> lock(clients_mtx);
	for (auto& sink : clients)
	{
		std::lock_guard<std::mutex> sl(sink->mtx);
		sink->done = true;
		sink->cv.notify_all();
	}
}

void ChannelSession::scheduleStop()
{
	int token  = ++stop_token;
	int linger = opts.linger_secs;
	TaskRegistry::global().spawn([self = shared_from_this(), token, linger]
	{
		std::this_thread::sleep_for(std::chrono::seconds(linger));
		if (self->stop_token.load() == token && self->client_count.load() == 0 && self->hlsIdle())
		{
			std::cerr << "[session:" << self->channel_id << "] linger expired, stopping\n";
			self->stop();
		}
	});
}