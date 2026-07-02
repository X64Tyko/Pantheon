#include "PreviewSession.h"
#include "EncoderArgs.h"
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

constexpr int kPreviewSegmentSecs = 2;
constexpr int kPreviewMaxHeight   = 480;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t computeOffset(const KairosNowResponse& item, int64_t atMs) {
    int64_t raw = std::max(int64_t(0), atMs - item.wall_clock_start_ms);
    return (item.is_filler && item.duration_ms > 0) ? raw % item.duration_ms : raw;
}

void appendPreviewOutputArgs(std::vector<std::string>& a, const std::string& dir) {
    a.insert(a.end(), {
        "-f", "hls",
        "-hls_time", std::to_string(kPreviewSegmentSecs),
        "-hls_list_size", "6",
        "-hls_flags", "delete_segments+append_list",
        "-hls_segment_filename", dir + "/seg-%05d.ts",
        dir + "/playlist.m3u8"
    });
}

std::vector<std::string> buildPreviewArgs(const std::string& ffmpeg_path,
                                           const KairosNowResponse& item,
                                           int64_t startOffsetMs,
                                           HwAccel hw_accel,
                                           const std::string& vaapi_device,
                                           const std::string& dir) {
    std::vector<std::string> a{ffmpeg_path, "-fflags", "+genpts+discardcorrupt", "-flags", "low_delay", "-re"};
    if (hw_accel == HwAccel::amd) a.insert(a.end(), {"-vaapi_device", vaapi_device});
    if (startOffsetMs > 0) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3) << (startOffsetMs / 1000.0);
        a.insert(a.end(), {"-ss", ss.str()});
    }
    pushHwAccelDecodeArgs(a, hw_accel);
    a.insert(a.end(), {"-i", item.file_path});
    a.insert(a.end(), {"-map", "0:v:0?", "-map", "0:a:0?", "-dn", "-map_chapters", "-1"});

    std::vector<std::string> vfParts{"scale=-2:min(ih\\," + std::to_string(kPreviewMaxHeight) + ")"};
    pushVideoEncoderArgs(a, vfParts, hw_accel, kPreviewSegmentSecs);
    pushVideoFilterArgs(a, vfParts);
    pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, /*audio_bitrate_kbps=*/96);

    appendPreviewOutputArgs(a, dir);
    return a;
}

std::vector<std::string> buildPreviewImageArgs(const std::string& ffmpeg_path,
                                                const std::string& image_path,
                                                HwAccel hw_accel,
                                                const std::string& vaapi_device,
                                                const std::string& dir) {
    std::vector<std::string> a{ffmpeg_path, "-fflags", "+genpts", "-flags", "low_delay", "-re"};
    if (hw_accel == HwAccel::amd) a.insert(a.end(), {"-vaapi_device", vaapi_device});
    a.insert(a.end(), {"-loop", "1", "-framerate", "25", "-i", image_path});
    a.insert(a.end(), {"-f", "lavfi", "-i", "anullsrc=channel_layout=stereo:sample_rate=48000"});
    a.insert(a.end(), {"-map", "0:v:0", "-map", "1:a:0", "-dn", "-map_chapters", "-1"});

    std::vector<std::string> vfParts{"scale=-2:min(ih\\," + std::to_string(kPreviewMaxHeight) + ")"};
    pushVideoEncoderArgs(a, vfParts, hw_accel, kPreviewSegmentSecs);
    pushVideoFilterArgs(a, vfParts);
    pushAudioEncoderArgs(a, /*loudnorm=*/false, /*speed=*/1.0, /*audio_bitrate_kbps=*/96);

    appendPreviewOutputArgs(a, dir);
    return a;
}

} // namespace

PreviewSession::PreviewSession(std::string session_id, std::string ffmpeg_path,
                                PreviewStreamOptions opts, KairosClient& kairos)
    : session_id(std::move(session_id)), ffmpeg_path(std::move(ffmpeg_path)),
      opts(std::move(opts)), kairos(kairos) {}

PreviewSession::~PreviewSession() { stop(); }

bool PreviewSession::switchChannel(const std::string& channel_id) {
    auto d = dir();
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    if (ec) {
        std::cerr << "[preview:" << session_id << "] failed to create \"" << d << "\": " << ec.message() << "\n";
        return false;
    }
    // Clear stale segments from whatever was previously playing so the
    // client never picks up a mix of old and new channel content.
    for (auto& entry : std::filesystem::directory_iterator(d))
        std::filesystem::remove(entry.path(), ec);

    auto item = kairos.getNow(channel_id);

    std::vector<std::string> args;
    if (!item || item->item_type == "offline") {
        std::string image = item && item->offline_image_path ? *item->offline_image_path : opts.default_logo_path;
        if (image.empty()) image = opts.default_logo_path;
        if (image.empty()) {
            std::cerr << "[preview:" << session_id << "] channel " << channel_id
                      << " is offline and no logo/default image is configured\n";
            return false;
        }
        std::cerr << "[preview:" << session_id << "] spawning ffmpeg (offline slate): \"" << image << "\"\n";
        args = buildPreviewImageArgs(ffmpeg_path, image, opts.hw_accel, opts.vaapi_device, d);
    } else if (item->file_path.empty()) {
        std::cerr << "[preview:" << session_id << "] channel " << channel_id << " /now item has no file_path\n";
        return false;
    } else {
        int64_t offset = computeOffset(*item, nowMs());
        std::cerr << "[preview:" << session_id << "] spawning ffmpeg: \"" << item->file_path << "\" offset=" << offset << "ms\n";
        args = buildPreviewArgs(ffmpeg_path, *item, offset, opts.hw_accel, opts.vaapi_device, d);
    }

    active = true;
    touch();

    std::lock_guard<std::mutex> lock(ffmpeg_mtx);
    if (ffmpeg) ffmpeg->kill();
    ffmpeg = std::make_unique<FfmpegProcess>(
        std::move(args),
        /*on_data=*/nullptr,
        [this](int code) { onExit(code); },
        opts.buffer_size,
        opts.ffmpeg_debug_logs
    );
    if (!ffmpeg->start()) {
        std::cerr << "[preview:" << session_id << "] failed to spawn ffmpeg\n";
        active = false;
        return false;
    }
    return true;
}

void PreviewSession::onExit(int code) {
    std::cerr << "[preview:" << session_id << "] ffmpeg exited (code=" << code << ")\n";
}

void PreviewSession::stop() {
    if (!active.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(ffmpeg_mtx);
        if (ffmpeg) { ffmpeg->kill(); ffmpeg.reset(); }
    }
    std::error_code ec;
    std::filesystem::remove_all(dir(), ec);
}

void PreviewSession::touch() { last_touch_ms.store(nowMs()); }

bool PreviewSession::isIdle() const {
    int64_t touch = last_touch_ms.load();
    if (touch == 0) return false;
    return (nowMs() - touch) > static_cast<int64_t>(opts.linger_secs) * 1000;
}
