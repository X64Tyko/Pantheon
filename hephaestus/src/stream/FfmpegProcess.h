#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

using DataCallback = std::function<void(const uint8_t*, size_t)>;
using ExitCallback = std::function<void(int)>; // exit code; -1 = signalled

class FfmpegProcess {
    std::vector<std::string> args;
    DataCallback on_data;
    ExitCallback on_exit;
    bool         log_stderr;
    bool         verbose;

    pid_t pid        = -1;
    int   stdout_fd  = -1;
    int   stderr_fd  = -1;
	int buffer_size = 1048576; // 1024 KB
    std::thread      reader_thread;
    std::thread      stderr_thread;
    std::atomic<bool> killed{false};

    // Last few KB of stderr, captured regardless of log_stderr, so a failed
    // exit can always print ffmpeg's actual reason instead of just a code.
    // Grown when verbose is on (see constructor) — the extra "-v verbose"
    // ffmpeg detail callers add in that mode is much chattier, and without a
    // bigger tail the actual failure reason gets pushed out by it.
    std::mutex  stderr_mtx;
    std::string stderr_tail;
    size_t      stderr_tail_max;
    static constexpr size_t kStderrTailMaxDefault = 4000;
    static constexpr size_t kStderrTailMaxVerbose = 32000;

public:
    // verbose: prints the full joined command line to std::cerr right before
    // spawning (in addition to the existing on-failure print), and grows the
    // stderr tail capture so verbose ffmpeg output ("-v verbose", pushed by
    // callers when their own verbose_transcode_logs option is set) doesn't
    // crowd out the real failure reason.
    FfmpegProcess(std::vector<std::string> args, DataCallback on_data, ExitCallback on_exit,
                  int buffer_size, bool log_stderr = false, bool verbose = false);
    ~FfmpegProcess();

    bool start();
    // SIGTERM, then blocks (briefly) until the process is actually gone,
    // escalating to SIGKILL if it doesn't exit promptly. Callers that kill
    // one session's ffmpeg and immediately spawn another for the same
    // limited hardware resource (NVENC/VAAPI session slots) rely on this
    // returning only once the slot is actually free. Destructor always
    // cleans up.
    void kill();

    FfmpegProcess(const FfmpegProcess&)            = delete;
    FfmpegProcess& operator=(const FfmpegProcess&) = delete;
};
