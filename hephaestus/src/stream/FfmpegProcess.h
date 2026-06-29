#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>

using DataCallback = std::function<void(const uint8_t*, size_t)>;
using ExitCallback = std::function<void(int)>; // exit code; -1 = signalled

class FfmpegProcess {
    std::vector<std::string> args;
    DataCallback on_data;
    ExitCallback on_exit;

    pid_t pid        = -1;
    int   stdout_fd  = -1;
    int   stderr_fd  = -1;
    std::thread      reader_thread;
    std::thread      stderr_thread;
    std::atomic<bool> killed{false};

public:
    FfmpegProcess(std::vector<std::string> args, DataCallback on_data, ExitCallback on_exit);
    ~FfmpegProcess();

    bool start();
    void kill(); // SIGTERM; destructor always cleans up

    FfmpegProcess(const FfmpegProcess&)            = delete;
    FfmpegProcess& operator=(const FfmpegProcess&) = delete;
};
