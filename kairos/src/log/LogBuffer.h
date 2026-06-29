#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

// ─── Ring buffer ──────────────────────────────────────────────────────────────

class LogBuffer {
public:
    static constexpr size_t kMax = 2000;

    void push(std::string line) {
        {
            std::lock_guard lock(mu_);
            entries_.push_back({seq_++, std::move(line)});
            if (entries_.size() > kMax) entries_.pop_front();
        }
        cv_.notify_all();
    }

    // Last `n` lines + sequence number of newest (0 if empty).
    std::pair<std::vector<std::string>, uint64_t> recent(size_t n) const {
        std::lock_guard lock(mu_);
        if (entries_.empty()) return {{}, 0};
        auto start = entries_.size() > n
                   ? entries_.end() - static_cast<std::ptrdiff_t>(n)
                   : entries_.begin();
        std::vector<std::string> out;
        out.reserve(static_cast<size_t>(entries_.end() - start));
        for (auto it = start; it != entries_.end(); ++it)
            out.push_back(it->text);
        return {out, entries_.back().seq};
    }

    // Blocks until new entries arrive after `after_seq`, or timeout elapses.
    std::pair<std::vector<std::string>, uint64_t>
    waitAfter(uint64_t after_seq, std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mu_);
        cv_.wait_until(lock,
            std::chrono::steady_clock::now() + timeout,
            [&] { return !entries_.empty() && entries_.back().seq > after_seq; });
        if (entries_.empty() || entries_.back().seq <= after_seq)
            return {{}, after_seq};
        std::vector<std::string> out;
        for (const auto& e : entries_)
            if (e.seq > after_seq) out.push_back(e.text);
        return {out, entries_.back().seq};
    }

private:
    struct Entry { uint64_t seq; std::string text; };

    mutable std::mutex              mu_;
    mutable std::condition_variable cv_;
    std::deque<Entry>               entries_;
    uint64_t                        seq_ = 1;  // 0 is the "nothing seen yet" sentinel
};

// ─── Tee streambuf: intercepts an ostream and pushes lines to LogBuffer ───────

class LogTee : public std::streambuf {
public:
    LogTee(std::ostream& target, LogBuffer& buf)
        : orig_(target.rdbuf()), buf_(buf), target_(target)
    {
        target_.rdbuf(this);
    }
    ~LogTee() { target_.rdbuf(orig_); }

protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return c;
        if (c == '\n') { buf_.push(line_); line_.clear(); }
        else           { line_ += static_cast<char>(c);  }
        return orig_->sputc(static_cast<char>(c));
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i) {
            char c = s[i];
            if (c == '\n') { buf_.push(line_); line_.clear(); }
            else           { line_ += c; }
        }
        return orig_->sputn(s, n);
    }

    // Forward flush() calls to the real underlying buffer so that std::endl
    // and explicit flush() calls actually drain the pipe in non-TTY environments
    // (Docker). Without this override, flush signals are silently dropped here.
    int sync() override { return orig_->pubsync(); }

private:
    std::streambuf* orig_;
    LogBuffer&      buf_;
    std::ostream&   target_;
    std::string     line_;
};
