#include "Config.h"
#include "api/Router.h"
#include "kairos/KairosClient.h"
#include "log/LogBuffer.h"
#include "stream/SessionManager.h"
#include <httplib.h>
#include <iostream>

int main(int argc, char* argv[]) {
    // Intercept cout/cerr before anything else so startup messages are captured.
    LogBuffer log_buffer;
    LogTee    tee_cout(std::cout, log_buffer);
    LogTee    tee_cerr(std::cerr, log_buffer);

    Config cfg = parseConfig(argc, argv);

    KairosClient kairos(cfg.kairos_url);

    StreamOptions stream_opts;
    stream_opts.ffprobe_path = cfg.ffprobe_path;
    stream_opts.audio_lang   = cfg.audio_lang;
    stream_opts.loudnorm     = cfg.loudnorm;
    stream_opts.linger_secs  = cfg.session_linger_secs;

    SessionManager sessions(kairos, cfg.ffmpeg_path, stream_opts);

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(16); };

    registerRoutes(svr, sessions, kairos, log_buffer, cfg);

    std::cout << "[hephaestus] listening on :" << cfg.port
              << "  kairos=" << cfg.kairos_url
              << "  ffmpeg=" << cfg.ffmpeg_path << "\n";

    svr.listen("0.0.0.0", cfg.port);
    return 0;
}
