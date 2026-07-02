#include "FfmpegProcess.h"
#include <iostream>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>
#include <chrono>

FfmpegProcess::FfmpegProcess(std::vector<std::string> args,
                              DataCallback on_data,
                              ExitCallback on_exit,
                              int buf_size,
                              bool log_stderr)
    : args(std::move(args))
    , on_data(std::move(on_data))
    , on_exit(std::move(on_exit))
	, buffer_size(buf_size)
    , log_stderr(log_stderr) {}

FfmpegProcess::~FfmpegProcess() {
    kill();
    if (stderr_thread.joinable()) stderr_thread.join();
    if (reader_thread.joinable()) reader_thread.join();
}

bool FfmpegProcess::start() {
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) == -1) return false;
    if (pipe(err_pipe) == -1) { close(out_pipe[0]); close(out_pipe[1]); return false; }

    // Read ends are not inherited by child
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);

    pid = fork();
    if (pid == -1) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // Child: wire stdout and stderr to their respective pipes
        close(out_pipe[0]);
        close(err_pipe[0]);
        if (dup2(out_pipe[1], STDOUT_FILENO) == -1) _exit(1);
        if (dup2(err_pipe[1], STDERR_FILENO) == -1) _exit(1);
        close(out_pipe[1]);
        close(err_pipe[1]);

        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(1);
    }

    // Parent
    close(out_pipe[1]);
    close(err_pipe[1]);
    stdout_fd = out_pipe[0];
    stderr_fd = err_pipe[0];

    // Drain ffmpeg stderr to avoid blocking the child process. When ffmpeg_debug_logs
    // is enabled (HEPH_FFMPEG_DEBUG=1) lines are emitted to std::cerr live, captured
    // by the LogTee in main and forwarded to /api/logs/stream in Hades. Regardless of
    // that flag, the last few KB are always kept in stderr_tail so a failed exit can
    // print ffmpeg's real reason (see reader_thread below) instead of just a code.
    stderr_thread = std::thread([this] {
        char buf[4096];
        std::string partial;
        while (true) {
            ssize_t n = read(stderr_fd, buf, sizeof(buf));
            if (n <= 0) break;
            {
                std::lock_guard<std::mutex> lock(stderr_mtx);
                stderr_tail.append(buf, static_cast<size_t>(n));
                if (stderr_tail.size() > kStderrTailMax)
                    stderr_tail.erase(0, stderr_tail.size() - kStderrTailMax);
            }
            if (!log_stderr) continue; // still captured above, just not streamed live
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    if (!partial.empty())
                        std::cerr << "[ffmpeg] " << partial << "\n";
                    partial.clear();
                } else {
                    partial += buf[i];
                }
            }
        }
        if (log_stderr && !partial.empty())
            std::cerr << "[ffmpeg] " << partial << "\n";
        // stderr_fd closed by kill()
    });

    reader_thread = std::thread([this] {
        uint8_t* buf = new uint8_t[buffer_size];
        while (true) {
            ssize_t n = read(stdout_fd, buf, buffer_size);
            if (n <= 0) break;
            if (on_data) on_data(buf, static_cast<size_t>(n));
        }
    	
    	delete[] buf;
        // stdout_fd closed by kill() — reader thread must not close it to
        // avoid a double-close race.

        int status = 0;
        waitpid(pid, &status, 0);
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        // Not joining stderr_thread here — the destructor already owns that
        // join, and doing it from both places races. stderr_tail is read
        // under its own mutex instead, which is safe even if stderr_thread
        // is still appending its last chunk (worst case: a few bytes short).
        if (code != 0 && !killed.load()) {
            std::string cmd;
            for (auto& a : args) { if (!cmd.empty()) cmd += ' '; cmd += a; }
            std::string tail;
            { std::lock_guard<std::mutex> lock(stderr_mtx); tail = stderr_tail; }
            std::cerr << "[ffmpeg] failed (code=" << code << "): " << cmd << "\n";
            if (!tail.empty()) std::cerr << "[ffmpeg] stderr:\n" << tail << "\n";
        }

        // Only fire on_exit for natural/crash exits, never for intentional kills.
        // When killed=true the session is already tearing down; calling on_exit
        // would race with stop() and could spawn a new ffmpeg after cleanup.
        if (on_exit && !killed.load()) {
            auto cb = on_exit;
            std::thread([cb, code] { cb(code); }).detach();
        }
    });

    return true;
}

void FfmpegProcess::kill() {
    if (killed.exchange(true)) return; // already killed
    if (pid > 0) ::kill(pid, SIGTERM);
    // Closing read ends unblocks the read() calls in reader_thread and stderr_thread
    if (stdout_fd != -1) { close(stdout_fd); stdout_fd = -1; }
    if (stderr_fd != -1) { close(stderr_fd); stderr_fd = -1; }

    // Don't return until the process is actually gone. reader_thread owns
    // the real waitpid() (reaping happens once its read() unblocks above),
    // so poll liveness with a signal-0 probe instead of racing it for the
    // child's exit status. A hardware encoder (NVENC/VAAPI) session isn't
    // freed until the process really exits, and callers like
    // PreviewSession::switchChannel() spawn a replacement immediately after
    // kill() returns, wanting that same limited slot.
    if (pid > 0) {
        constexpr int kGraceMs = 2000, kPollMs = 50;
        int waited = 0;
        while (waited < kGraceMs && ::kill(pid, 0) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            waited += kPollMs;
        }
        if (::kill(pid, 0) == 0) ::kill(pid, SIGKILL);
    }
}
