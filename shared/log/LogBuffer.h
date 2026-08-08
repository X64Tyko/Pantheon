#pragma once
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

// Shared by kairos, hermes and hephaestus (see shared/README.md). Mechanics
// (ring buffer, file rotation, tee-streambuf) live here; each service's own
// category-prefix filtering policy over its own runtime flags is supplied
// via setFilter() rather than baked in, since that policy differs per
// service and depends on globals this header has no business knowing about.

// Wall-clock timestamp prefixed onto every line pushed through this buffer
// (see push() below) — without it, only ffmpeg's own relayed stderr lines
// (FfmpegProcess stamps those itself) carried any time info at all, so a
// downloaded log had no way to tell whether a "[session:...]"/"[splicer]"/
// etc. line happened before, during, or after a given ffmpeg event, which
// matters for diagnosing races between session lifecycle and still-running
// ffmpeg processes. Local time, millisecond precision, date included since
// downloaded logs can span midnight or multiple days.
inline std::string logTimestampNow()
{
	using namespace std::chrono;
	auto now      = system_clock::now();
	auto ms       = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
	std::time_t t = system_clock::to_time_t(now);
	std::tm tm{};
	localtime_r(&t, &tm);
	std::ostringstream ss;
	ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
	return ss.str();
}

// ─── Ring buffer ──────────────────────────────────────────────────────────────

class LogBuffer
{
public:
	static constexpr size_t kMax         = 2000;
	static constexpr size_t kMaxFileSize = 10 * 1024 * 1024; // 10 MB

	void setFile(const std::string& path)
	{
		std::lock_guard lock(mu_);
		path_ = path;
		openFile();
	}

	// The on-disk path passed to setFile(), or empty if none — lets a caller
	// (e.g. a diagnostics log-export endpoint) find the *complete* log
	// history without duplicating this service's own hardcoded path. Every
	// line reaches the file regardless of setFilter()'s policy, unlike the
	// in-memory ring buffer recent()/waitAfter() serve, so this is the
	// authoritative source for "all logs," not just the last kMax entries.
	std::string path() const
	{
		std::lock_guard lock(mu_);
		return path_;
	}

	// Decides which lines reach the in-memory ring buffer (and therefore the
	// Hades /api/logs/stream UI, and any forward target — see setForward).
	// The file always gets every line regardless. No filter set means
	// everything passes. Set once at startup, before any push() call, so it
	// needs no locking of its own.
	void setFilter(std::function<bool(const std::string &)> filter)
	{
		filter_ = std::move(filter);
	}

	// Lines that pass this buffer's own filter are also pushed into fwd (if
	// set). Hermes uses this to keep a file-backed, locally-scoped LogBuffer
	// (its own [hermes]/[roku-ecp]/etc lines) separate from the no-file
	// "combined" LogBuffer that /api/logs/stream actually serves to Hades —
	// which also receives Kairos's and Hephaestus's relayed streams — so
	// relaying someone else's already-logged lines through Hermes doesn't
	// also duplicate them into hermes.log. Set once at startup, before any
	// push() call, so it needs no locking of its own.
	void setForward(LogBuffer* fwd) { fwd_ = fwd; }

	void push(std::string line)
	{
		// Filtering/forwarding operate on the original, category-tagged line
		// (setFilter() callbacks match on a leading "[category]" — see e.g.
		// hephaestusLogFilter — which a leading timestamp would break) — only
		// the file/ring-buffer copy actually stored gets stamped.
		bool should_push    = !filter_ || filter_(line);
		std::string stamped = logTimestampNow() + " " + line;
		{
			std::lock_guard lock(mu_);
			if (file_)
			{
				file_ << stamped << std::endl;
				bytes_written_ += stamped.size() + 1;
				if (bytes_written_ > kMaxFileSize)
				{
					rotateFile();
				}
			}

			if (should_push)
			{
				if (fwd_) fwd_->push(line); // copy of the original — fwd_ stamps it independently on its own push()
				entries_.push_back({seq_++, std::move(stamped)});
				if (entries_.size() > kMax) entries_.pop_front();
			}
		}
		cv_.notify_all();
	}

	// Last `n` lines + sequence number of newest (0 if empty).
	std::pair<std::vector<std::string>, uint64_t> recent(size_t n) const
	{
		std::lock_guard lock(mu_);
		if (entries_.empty()) return {{}, 0};
		auto start = entries_.size() > n
						 ? entries_.end() - static_cast<std::ptrdiff_t>(n)
						 : entries_.begin();
		std::vector<std::string> out;
		out.reserve(static_cast<size_t>(entries_.end() - start));
		for (auto it = start; it != entries_.end(); ++it) out.push_back(it->text);
		return {out, entries_.back().seq};
	}

	// Blocks until new entries arrive after `after_seq`, or timeout elapses.
	std::pair<std::vector<std::string>, uint64_t> waitAfter(uint64_t after_seq, std::chrono::milliseconds timeout) const
	{
		std::unique_lock lock(mu_);
		cv_.wait_until(lock,
					   std::chrono::steady_clock::now() + timeout,
					   [&] { return !entries_.empty() && entries_.back().seq > after_seq; });
		if (entries_.empty() || entries_.back().seq <= after_seq) return {{}, after_seq};
		std::vector<std::string> out;
		for (const auto& e : entries_) if (e.seq > after_seq) out.push_back(e.text);
		return {out, entries_.back().seq};
	}

private:
	struct Entry
	{
		uint64_t seq;
		std::string text;
	};

	mutable std::mutex mu_;
	mutable std::condition_variable cv_;
	std::deque<Entry> entries_;
	std::ofstream file_;
	std::string path_;
	size_t bytes_written_ = 0;
	uint64_t seq_         = 1; // 0 is the "nothing seen yet" sentinel
	std::function<bool(const std::string &)> filter_;
	LogBuffer* fwd_ = nullptr;

	void openFile()
	{
		if (path_.empty()) return;
		// The parent directory not existing yet used to make this fail
		// silently — every line just quietly stopped reaching the file with
		// no further retry, since nothing calls openFile() again afterward.
		std::error_code ec;
		auto parent = std::filesystem::path(path_).parent_path();
		if (!parent.empty()) std::filesystem::create_directories(parent, ec);
		file_.open(path_, std::ios::app);
		if (file_)
		{
			file_.seekp(0, std::ios::end);
			bytes_written_ = static_cast<size_t>(file_.tellp());
		}
		else
		{
			std::cerr << "[log] failed to open log file: " << path_ << "\n";
		}
	}

	void rotateFile()
	{
		file_.close();
		std::string old_path = path_ + ".1";
		std::remove(old_path.c_str());
		std::rename(path_.c_str(), old_path.c_str());
		openFile();
	}
};

// ─── Tee streambuf: intercepts an ostream and pushes lines to LogBuffer ───────

class LogTee : public std::streambuf
{
public:
	LogTee(std::ostream& target, LogBuffer& buf)
		: orig_(target.rdbuf())
		, buf_(buf)
		, target_(target)
	{
		target_.rdbuf(this);
	}

	~LogTee() { target_.rdbuf(orig_); }

protected:
	int overflow(int c) override
	{
		if (c == traits_type::eof()) return c;
		if (c == '\n')
		{
			buf_.push(line_);
			line_.clear();
		}
		else { line_ += static_cast<char>(c); }
		return orig_->sputc(static_cast<char>(c));
	}

	std::streamsize xsputn(const char* s, std::streamsize n) override
	{
		for (std::streamsize i = 0; i < n; ++i)
		{
			char c = s[i];
			if (c == '\n')
			{
				buf_.push(line_);
				line_.clear();
			}
			else { line_ += c; }
		}
		return orig_->sputn(s, n);
	}

	// Forward flush() calls to the real underlying buffer so that std::endl
	// and explicit flush() calls actually drain the pipe in non-TTY environments
	// (Docker). Without this override, flush signals are silently dropped here.
	int sync() override { return orig_->pubsync(); }

private:
	std::streambuf* orig_;
	LogBuffer& buf_;
	std::ostream& target_;
	std::string line_;
};