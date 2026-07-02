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

    pid_t pid        = -1;
    int   stdout_fd  = -1;
    int   stderr_fd  = -1;
	int buffer_size = 1048576; // 1024 KB
    std::thread      reader_thread;
    std::thread      stderr_thread;
    std::atomic<bool> killed{false};

    // Last few KB of stderr, captured regardless of log_stderr, so a failed
    // exit can always print ffmpeg's actual reason instead of just a code.
    std::mutex  stderr_mtx;
    std::string stderr_tail;
    static constexpr size_t kStderrTailMax = 4000;

public:
    FfmpegProcess(std::vector<std::string> args, DataCallback on_data, ExitCallback on_exit,
                  int buffer_size, bool log_stderr = false);
    ~FfmpegProcess();

    bool start();
    void kill(); // SIGTERM; destructor always cleans up

    FfmpegProcess(const FfmpegProcess&)            = delete;
    FfmpegProcess& operator=(const FfmpegProcess&) = delete;
};
