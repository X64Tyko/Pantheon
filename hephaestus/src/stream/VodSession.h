#pragma once
#include "ChannelSession.h" // HwAccel
#include "ClientCapabilities.h"
#include "FfmpegProcess.h"
#include "MediaProbe.h"
#include "model/ExternalSubtitle.h" // shared/ — see that header for why
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct VodStreamOptions {
    std::string ffprobe_path = "ffprobe";
    // VOD sessions live at "<hls_root>/vod/<session_id>/". Never empty in
    // practice — VOD has no non-HLS fallback the way live channels do.
    std::string hls_root;
    int         linger_secs       = 60;
    int         buffer_size       = 1048576;
    HwAccel     hw_accel          = HwAccel::none; // resolved encode backend (HwProbe)
    // Resolved decode backend + which source video codecs it can hwaccel-
    // decode, from HwProbe::probeHwCapabilities() at startup. Independent of
    // hw_accel above -- see EncoderArgs.h's pushHwAccelDecodeArgs.
    HwAccel     decode_hw_accel   = HwAccel::none;
    std::set<std::string> decodable_codecs;
    std::string vaapi_device      = "/dev/dri/renderD128";
    bool        ffmpeg_debug_logs = false;
    bool        verbose_transcode_logs = false; // -v verbose + full command line on every spawn
};

// One file, one viewer. Unlike ChannelSession there's no scheduling and no
// client fan-out — ffmpeg writes HLS segments straight to disk and the HTTP
// layer serves them as static files. "Seek" and "switch track" are both
// implemented by the caller tearing down the session and calling start()
// again at the new position/track — see VodSessionManager and the Hephaestus
// Router's /stream/vod/start handler.
class VodSession {
public:
    VodSession(std::string session_id, std::string ffmpeg_path, VodStreamOptions opts);
    ~VodSession();

    VodSession(const VodSession&)            = delete;
    VodSession& operator=(const VodSession&) = delete;

    // Probes file_path, decides direct-play vs transcode, and spawns ffmpeg
    // writing an HLS VOD playlist to dir(). Returns false if probing fails
    // (file missing/unreadable) or ffmpeg won't start. hdr_capable comes from
    // the requesting client's own display capability check (see
    // api/client.ts's isHdrCapableDisplay) — an HDR source gets a real
    // HEVC Main10 HDR10 re-encode when true, or a real tone-map to correct
    // SDR when false (see EncoderArgs.cpp's pushVideoEncoderArgs).
    // client_caps is this specific client's declared decode capability (see
    // ClientCapabilities.h) — nullopt when the requesting token never
    // declared one (or Hephaestus restarted since), in which case
    // isDirectPlayable() falls back to the conservative h264/aac allowlist.
    // external_subtitles is the full catalog for this item (from Kairos's
    // playback-info response) — subtitle_track <= -2 selects
    // external_subtitles[-(subtitle_track)-2] (see Router.cpp's
    // /stream/vod/start handler for where that negative-index scheme is
    // assigned; both sides must stay in sync since nothing else enforces
    // it). >= 0 still means an embedded track's relative_index; -1 means no
    // subtitle at all.
    bool start(const std::string& file_path, int64_t position_ms,
               int audio_track, int subtitle_track, bool hdr_capable,
               const std::optional<ClientCapabilities>& client_caps,
               const std::vector<ExternalSubtitle>& external_subtitles = {});

    void stop();
    // Called by the HTTP handler on every playlist/segment GET.
    void touch();

    bool isActive() const { return active.load(); }
    bool isIdle() const;
    const std::string& sessionId() const { return session_id; }
    std::string dir() const { return opts.hls_root + "/vod/" + session_id; }
    bool directPlay() const { return direct_play; }
    const MediaInfo& tracks() const { return media_info; }
    bool hasSubtitleOutput() const { return subtitle_output; }
    // True when the selected subtitle track is a bitmap format (PGS/DVD/DVB)
    // being composited directly onto the video via ffmpeg's overlay filter,
    // rather than extracted as a WebVTT sidecar (hasSubtitleOutput()/
    // subtitle_url) — no separate subtitle URL exists for this case, it's
    // baked into the HLS video segments themselves.
    bool subtitleBurnedIn() const { return subtitle_burn_in; }
    // The full external-subtitle catalog this session was started with (not
    // just whichever one is selected) — Router.cpp lists all of them in
    // tracks.subtitles[] alongside the embedded ones.
    const std::vector<ExternalSubtitle>& externalSubtitles() const { return external_subtitles_; }
    // ffmpeg writes subs.vtt as one flat file (no incremental HLS-style
    // segmenting the way video/audio gets), so the file exists on disk
    // almost immediately (avio_open2 happens before any cues are muxed) but
    // isn't actually complete until the whole process closes it — see
    // Router.cpp's /subs.vtt handler, which waits on this rather than mere
    // file existence before serving (a <track> element fetches its VTT
    // resource exactly once and never re-fetches, so serving it early means
    // permanently missing every cue after that point).
    bool hasFfmpegExited() const { return ffmpeg_exited.load(); }

    // For the activity/debugging view (ActivityRouter).
    const std::string& filePath() const { return file_path; }
    HwAccel hwAccel() const       { return opts.hw_accel; }
    HwAccel decodeHwAccel() const { return opts.decode_hw_accel; }
    int64_t startedAtMs() const   { return started_at_ms; }

private:
    std::string   session_id;
    std::string   ffmpeg_path;
    VodStreamOptions opts;

    std::mutex    ffmpeg_mtx;
    std::unique_ptr<FfmpegProcess> ffmpeg;

    std::atomic<bool>    active{false};
    std::atomic<bool>    ffmpeg_exited{false};
    std::atomic<int64_t> last_touch_ms{0};
    bool          direct_play     = false;
    bool          subtitle_output = false;
    bool          subtitle_burn_in = false;
    MediaInfo     media_info;
    std::string   file_path;
    int64_t       started_at_ms = 0;
    std::vector<ExternalSubtitle> external_subtitles_;

    void onExit(int code);
};
