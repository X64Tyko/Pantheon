#pragma once
#include "ChannelSession.h" // HwAccel
#include "FfmpegProcess.h"
#include "../kairos/KairosClient.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>

struct PreviewStreamOptions
{
	std::string ffprobe_path = "ffprobe";
	std::string hls_root;
	std::string default_logo_path;
	int linger_secs  = 60;
	int buffer_size  = 1048576;
	HwAccel hw_accel = HwAccel::none; // resolved encode backend (HwProbe)
	// Resolved decode backend + which source video codecs it can hwaccel-
	// decode, from HwProbe::probeHwCapabilities() at startup. Independent of
	// hw_accel above -- see EncoderArgs.h's pushHwAccelDecodeArgs.
	HwAccel decode_hw_accel = HwAccel::none;
	std::set<std::string> decodable_codecs;
	std::string vaapi_device    = "/dev/dri/renderD128";
	bool ffmpeg_debug_logs      = false;
	bool verbose_transcode_logs = false; // -v verbose + full command line on every spawn
	// Host-wide hardware-encode-session cap shared with channels/VOD — see
	// EncoderAdmission's own class comment. nullptr (default) disables
	// gating entirely, same as every other caller of it.
	EncoderAdmission* encoder_admission = nullptr;
	// In-memory segment cache shared with channels/VOD — nullptr (default)
	// disables caching entirely.
	SegmentCache* segment_cache = nullptr;
};

// Manifest URL is stable for the session's whole life — switchChannel()
// replaces the underlying ffmpeg process, not the session, so the client
// never remounts its player when the previewed channel changes.
class PreviewSession
{
public:
	// hdr_capable is fixed for the session's whole life (like hw_accel in
	// opts) — set once from the requesting client's own display capability
	// check (see api/client.ts's isHdrCapableDisplay) at the initial start,
	// and reused by every later switchChannel() on this same session since
	// /stream/preview/:id/switch has no reason to re-send it.
	PreviewSession(std::string session_id, std::string ffmpeg_path,
				   PreviewStreamOptions opts, KairosClient& kairos, bool hdr_capable);
	~PreviewSession();

	PreviewSession(const PreviewSession&)            = delete;
	PreviewSession& operator=(const PreviewSession&) = delete;

	bool switchChannel(const std::string& channel_id);

	void stop();
	void touch();

	bool isActive() const { return active.load(); }
	bool isIdle() const;
	const std::string& sessionId() const { return session_id; }
	std::string dir() const { return opts.hls_root + "/preview/" + session_id; }

	// Exposed for testing — see computeOffset's own comment.
	static int64_t computeOffsetForTest(const KairosNowResponse& item, int64_t atMs)
	{
		return computeOffset(item, atMs);
	}

private:
	// Computes how far into `item` playback should start, given the true
	// wall-clock time `atMs` — mirrors ChannelSession::computeOffset (kept as
	// its own copy, not shared, since preview sessions have no speed-ramp
	// concept to reconcile with). Loops fillers on their own duration; clamps
	// to 0 for non-fillers that haven't started yet.
	static int64_t computeOffset(const KairosNowResponse& item, int64_t atMs);
	std::string session_id;
	std::string ffmpeg_path;
	PreviewStreamOptions opts;
	KairosClient& kairos;
	bool hdr_capable;

	std::mutex ffmpeg_mtx;
	std::unique_ptr<FfmpegProcess> ffmpeg;

	std::atomic<bool> active{false};
	std::atomic<int64_t> last_touch_ms{0};

	void onExit(int code);
};