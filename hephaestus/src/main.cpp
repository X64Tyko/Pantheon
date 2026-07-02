#include "Config.h"
#include "api/Router.h"
#include "kairos/KairosClient.h"
#include "log/LogBuffer.h"
#include "stream/EncoderArgs.h" // hwAccelName
#include "stream/HwProbe.h"
#include "stream/SessionManager.h"
#include "stream/VodSessionManager.h"
#include "stream/PreviewSessionManager.h"
#include <httplib.h>
#include <iostream>

int main(int argc, char* argv[]) {
    // Intercept cout/cerr before anything else so startup messages are captured.
    LogBuffer log_buffer;
    LogTee    tee_cout(std::cout, log_buffer);
    LogTee    tee_cerr(std::cerr, log_buffer);

    Config cfg = parseConfig(argc, argv);

    // Verifies the configured GPU backend actually works before any session
    // is ever built with it, instead of every stream discovering that the
    // hard way at spawn time. Resolved once, here, and copied into every
    // *StreamOptions below — sessions never re-probe or retry.
    HwCapabilities hw_caps = probeHwCapabilities(cfg.hw_accel, cfg.ffmpeg_path,
                                                  cfg.vaapi_device, cfg.hw_probe_assets_dir);
    std::cout << "[hephaestus] hw-accel: requested=" << hwAccelName(cfg.hw_accel)
              << " encode=" << hwAccelName(hw_caps.encode)
              << " decode=" << hwAccelName(hw_caps.decode)
              << " decodable_codecs=" << hw_caps.decodable_codecs.size() << "\n";

    KairosClient kairos(cfg.kairos_url);

    StreamOptions stream_opts;
    stream_opts.ffprobe_path = cfg.ffprobe_path;
    stream_opts.audio_lang   = cfg.audio_lang;
    stream_opts.loudnorm          = cfg.loudnorm;
    stream_opts.ffmpeg_debug_logs = cfg.ffmpeg_debug_logs;
    stream_opts.verbose_transcode_logs = cfg.verbose_transcode_logs;
    stream_opts.linger_secs       = cfg.session_linger_secs;
    stream_opts.buffer_size       = cfg.stream_buffer_size;
    stream_opts.hw_accel          = hw_caps.encode;
    stream_opts.decode_hw_accel   = hw_caps.decode;
    stream_opts.decodable_codecs  = hw_caps.decodable_codecs;
    stream_opts.vaapi_device = cfg.vaapi_device;
    stream_opts.default_logo_path = cfg.default_logo_path;
    stream_opts.hls_root     = cfg.hls_root;

    // stream_opts.buffer_size (from --buffer-size/BUF_SIZE above) is the fallback
    // if Kairos is unreachable — SessionManager fetches the persisted Kairos
    // setting itself (and keeps it fresh in the background) so later changes
    // made in Hades don't require a Hephaestus restart.
    SessionManager sessions(kairos, cfg.ffmpeg_path, stream_opts);

    VodStreamOptions vod_opts;
    vod_opts.ffprobe_path      = cfg.ffprobe_path;
    vod_opts.hls_root          = cfg.hls_root;
    vod_opts.linger_secs       = cfg.session_linger_secs;
    vod_opts.buffer_size       = cfg.stream_buffer_size;
    vod_opts.hw_accel          = hw_caps.encode;
    vod_opts.decode_hw_accel   = hw_caps.decode;
    vod_opts.decodable_codecs  = hw_caps.decodable_codecs;
    vod_opts.vaapi_device      = cfg.vaapi_device;
    vod_opts.ffmpeg_debug_logs = cfg.ffmpeg_debug_logs;
    vod_opts.verbose_transcode_logs = cfg.verbose_transcode_logs;
    VodSessionManager vodSessions(cfg.ffmpeg_path, vod_opts);

    PreviewStreamOptions preview_opts;
    preview_opts.ffprobe_path      = cfg.ffprobe_path;
    preview_opts.hls_root          = cfg.hls_root;
    preview_opts.default_logo_path = cfg.default_logo_path;
    preview_opts.linger_secs       = cfg.session_linger_secs;
    preview_opts.buffer_size       = cfg.stream_buffer_size;
    preview_opts.hw_accel          = hw_caps.encode;
    preview_opts.decode_hw_accel   = hw_caps.decode;
    preview_opts.decodable_codecs  = hw_caps.decodable_codecs;
    preview_opts.vaapi_device      = cfg.vaapi_device;
    preview_opts.ffmpeg_debug_logs = cfg.ffmpeg_debug_logs;
    preview_opts.verbose_transcode_logs = cfg.verbose_transcode_logs;
    PreviewSessionManager previewSessions(cfg.ffmpeg_path, preview_opts, kairos);

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(16); };

    registerRoutes(svr, sessions, vodSessions, previewSessions, kairos, log_buffer, cfg);

    std::cout << "[hephaestus] listening on :" << cfg.port
              << "  kairos=" << cfg.kairos_url
              << "  ffmpeg=" << cfg.ffmpeg_path << "\n";

    svr.listen("0.0.0.0", cfg.port);
    return 0;
}
