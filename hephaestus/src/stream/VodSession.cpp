#include "VodSession.h"
#include "EncoderArgs.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <filesystem>

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Shared by buildVodArgs' -hls_time and pushVideoEncoderArgs'
// -force_key_frames — see the matching constant/comment in ChannelSession.cpp
// for why these must stay in sync (HLS can only cut a segment at a keyframe).
static constexpr int kVodHlsSegmentSecs = 6;

// Text-based subtitle codecs ffmpeg can transcode to WebVTT for an HLS
// sidecar track. Bitmap/graphic formats (PGS, DVD, DVB) aren't extractable
// this way — out of scope for v1 (see plan).
static bool isTextSubtitleCodec(const std::string& codec) {
    return codec == "subrip" || codec == "ass" || codec == "ssa" ||
           codec == "mov_text" || codec == "webvtt" || codec == "text";
}

// h264/aac is the conservative "every browser can play this without
// transcoding" allowlist. Anything else (hevc, av1, ac3, dts, ...) transcodes.
static bool isDirectPlayable(const MediaInfo& info, int audioTrack) {
    if (info.video.empty() || info.video[0].codec != "h264") return false;
    auto it = std::find_if(info.audio.begin(), info.audio.end(),
        [&](const AudioTrack& t) { return t.relative_index == audioTrack; });
    return it != info.audio.end() && it->codec == "aac";
}

static std::vector<std::string> buildVodArgs(
    const std::string& ffmpeg_path,
    const std::string& file_path,
    int64_t positionMs,
    int audioTrack,
    int subtitleTrack,
    bool directPlay,
    bool subtitleOutput,
    HwAccel hw_accel,
    const std::string& vaapi_device,
    const std::string& dir,
    double fps)
{
    std::vector<std::string> a;
    a.push_back(ffmpeg_path);

    a.insert(a.end(), {"-fflags", "+genpts+discardcorrupt"});

    if (hw_accel == HwAccel::amd)
        a.insert(a.end(), {"-vaapi_device", vaapi_device});

    if (positionMs > 0) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3) << (positionMs / 1000.0);
        a.push_back("-ss"); a.push_back(ss.str());
    }

    // Direct-play is a pure stream copy — nothing gets decoded, so a decode
    // hwaccel would be a pointless no-op at best.
    if (!directPlay) pushHwAccelDecodeArgs(a, hw_accel);

    a.push_back("-i"); a.push_back(file_path);

    a.insert(a.end(), {"-map", "0:v:0?", "-map", "0:a:" + std::to_string(audioTrack) + "?"});
    a.insert(a.end(), {"-dn", "-map_chapters", "-1"});

    if (directPlay) {
        a.insert(a.end(), {"-c:v", "copy", "-c:a", "copy"});
    } else {
        std::vector<std::string> vfParts;
        pushVideoEncoderArgs(a, vfParts, hw_accel, kVodHlsSegmentSecs, fps);
        pushVideoFilterArgs(a, vfParts);
        pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, /*audio_bitrate_kbps=*/192);
    }

    a.insert(a.end(), {
        "-f", "hls",
        "-hls_time", std::to_string(kVodHlsSegmentSecs),
        "-hls_playlist_type", "vod",
        "-hls_list_size", "0",
        "-hls_segment_filename", dir + "/seg-%05d.ts",
        dir + "/playlist.m3u8"
    });

    // Second output group in the same ffmpeg process: the selected text
    // subtitle track, transcoded to a WebVTT sidecar.
    if (subtitleOutput)
        a.insert(a.end(), {"-map", "0:s:" + std::to_string(subtitleTrack), "-c:s", "webvtt",
                            dir + "/subs.vtt"});

    return a;
}

VodSession::VodSession(std::string session_id, std::string ffmpeg_path, VodStreamOptions opts)
    : session_id(std::move(session_id)), ffmpeg_path(std::move(ffmpeg_path)), opts(std::move(opts)) {}

VodSession::~VodSession() { stop(); }

bool VodSession::start(const std::string& file_path, int64_t position_ms,
                        int audio_track, int subtitle_track) {
    auto info = probeMedia(opts.ffprobe_path, file_path);
    if (!info) {
        std::cerr << "[vod:" << session_id << "] probe failed for \"" << file_path << "\"\n";
        return false;
    }
    media_info = *info;

    if (audio_track < 0) audio_track = pickAudioTrack(media_info, "");
    direct_play = isDirectPlayable(media_info, audio_track);

    subtitle_output = false;
    if (subtitle_track >= 0) {
        auto it = std::find_if(media_info.subtitles.begin(), media_info.subtitles.end(),
            [&](const SubtitleTrack& t) { return t.relative_index == subtitle_track; });
        subtitle_output = it != media_info.subtitles.end() && isTextSubtitleCodec(it->codec);
    }

    auto d = dir();
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    if (ec) {
        std::cerr << "[vod:" << session_id << "] failed to create \"" << d << "\": " << ec.message() << "\n";
        return false;
    }

    double fps = media_info.video.empty() ? 0 : media_info.video[0].fps;
    auto args = buildVodArgs(ffmpeg_path, file_path, position_ms, audio_track, subtitle_track,
                              direct_play, subtitle_output, opts.hw_accel, opts.vaapi_device, d, fps);

    std::cerr << "[vod:" << session_id << "] spawning ffmpeg: \"" << file_path << "\""
              << " offset=" << position_ms << "ms direct_play=" << (direct_play ? "yes" : "no") << "\n";

    active = true;
    touch();

    std::lock_guard<std::mutex> lock(ffmpeg_mtx);
    ffmpeg = std::make_unique<FfmpegProcess>(
        std::move(args),
        /*on_data=*/nullptr, // output goes to disk, not stdout — nothing to fan out
        [this](int code) { onExit(code); },
        opts.buffer_size,
        opts.ffmpeg_debug_logs
    );
    if (!ffmpeg->start()) {
        std::cerr << "[vod:" << session_id << "] failed to spawn ffmpeg\n";
        active = false;
        return false;
    }
    return true;
}

void VodSession::onExit(int code) {
    std::cerr << "[vod:" << session_id << "] ffmpeg exited (code=" << code << ")\n";
    // Natural completion or a crash both just end the session — VOD has no
    // "next item" to transition to the way a channel does.
}

void VodSession::stop() {
    if (!active.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(ffmpeg_mtx);
        if (ffmpeg) { ffmpeg->kill(); ffmpeg.reset(); }
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
