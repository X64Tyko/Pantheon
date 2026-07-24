#pragma once
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include "log/LogBuffer.h" // for the recent-lines dump in onTerminate

// Local-only crash/exception capture, shared by kairos/hermes/hephaestus
// (see log/LogBuffer.h, metrics/OperationMetrics.h, thread/TaskRegistry.h for
// the same shared/ convention). Today an unhandled C++ exception or a fatal
// signal just kills the process silently — no stack trace, no timestamp, no
// record of what it was doing. This writes a plain local marker file to
// data_dir before letting the process die exactly as it would have without
// this (std::abort() / re-raising the signal with its default disposition)
// — it only adds "something noticed and recorded it," not recovery, and
// nothing here is ever sent anywhere: the marker is a local file the admin's
// own browser fetches from their own server's activity/debugging view (see
// each service's ActivityService/ActivityRouter), consistent with Pantheon's
// "nothing leaves this installation" telemetry model.
//
// The signal handler (onFatalSignal) is deliberately minimal — only
// async-signal-safe calls (open/write/close/raise, hand-rolled integer-to-
// ASCII, no malloc/iostream/snprintf, whose async-signal-safety POSIX
// doesn't guarantee) — a best-effort "it crashed, roughly when, which
// service" marker, not a full backtrace/symbol-resolution tool. A real
// backtrace (libbacktrace, or backtrace()/backtrace_symbols_fd() — the
// _fd variant *is* async-signal-safe, unlike backtrace_symbols()) is a
// reasonable follow-up, not required for this to be useful.
//
// std::terminate (onTerminate), by contrast, normally runs on the unwinding
// thread during ordinary stack unwinding — NOT inside a signal handler — so
// it's safe to do more there: pull the exception's what() and the last few
// LogBuffer lines via fopen/fprintf.
namespace crash_handler {

inline std::string& dataDirRef() { static std::string dir; return dir; }
inline std::string& serviceNameRef() { static std::string name; return name; }
inline LogBuffer*& logBufferRef() { static LogBuffer* buf = nullptr; return buf; }

inline std::string crashMarkerPath() {
	return dataDirRef() + "/crash-" + serviceNameRef() + ".txt";
}

// Async-signal-safe decimal integer -> ASCII, appended into `out` (capped at
// `cap`), returning the new write position.
inline size_t appendInt(char* out, size_t pos, size_t cap, long long v) {
	char tmp[24]; int t = 0;
	bool neg = v < 0;
	unsigned long long uv = neg ? static_cast<unsigned long long>(-v) : static_cast<unsigned long long>(v);
	if (uv == 0) tmp[t++] = '0';
	while (uv > 0 && t < 24) { tmp[t++] = char('0' + (uv % 10)); uv /= 10; }
	if (neg && pos < cap) out[pos++] = '-';
	while (t > 0 && pos < cap) out[pos++] = tmp[--t];
	return pos;
}

inline size_t appendStr(char* out, size_t pos, size_t cap, const char* s) {
	while (*s && pos < cap) out[pos++] = *s++;
	return pos;
}

inline void onFatalSignal(int sig) {
	const std::string& dir = dataDirRef();
	if (!dir.empty()) {
		int fd = ::open(crashMarkerPath().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			char buf[256];
			size_t pos = 0;
			pos = appendStr(buf, pos, sizeof(buf), "service=");
			pos = appendStr(buf, pos, sizeof(buf), serviceNameRef().c_str());
			pos = appendStr(buf, pos, sizeof(buf), " reason=signal signal=");
			pos = appendInt(buf, pos, sizeof(buf), sig);
			pos = appendStr(buf, pos, sizeof(buf), " epoch_secs=");
			pos = appendInt(buf, pos, sizeof(buf), static_cast<long long>(time(nullptr)));
			pos = appendStr(buf, pos, sizeof(buf), "\n");
			::write(fd, buf, pos);
			::close(fd);
		}
	}
	::signal(sig, SIG_DFL);
	::raise(sig);
}

inline void onTerminate() {
	std::string reason = "unknown (no active exception)";
	if (auto eptr = std::current_exception()) {
		try { std::rethrow_exception(eptr); }
		catch (const std::exception& e) { reason = e.what(); }
		catch (...) { reason = "non-std::exception thrown"; }
	}
	if (!dataDirRef().empty()) {
		if (FILE* f = fopen(crashMarkerPath().c_str(), "w")) {
			fprintf(f, "service=%s reason=uncaught_exception what=%s epoch_secs=%lld\n",
					serviceNameRef().c_str(), reason.c_str(), static_cast<long long>(time(nullptr)));
			if (LogBuffer* log = logBufferRef()) {
				fprintf(f, "--- last log lines ---\n");
				for (auto& line : log->recent(30).first) fprintf(f, "%s\n", line.c_str());
			}
			fclose(f);
		}
	}
	// Reset SIGABRT to its default disposition first: std::abort() raises
	// SIGABRT, which our own onFatalSignal handler (installed below) would
	// otherwise catch and immediately overwrite the detailed marker just
	// written above with its own bare "reason=signal" one. A genuine raw
	// SIGABRT NOT coming through here (an assert() failure, some other
	// direct abort()) is unaffected — it still hits onFatalSignal normally.
	::signal(SIGABRT, SIG_DFL);
	std::abort();
}

} // namespace crash_handler

// Call once at the very top of main(), right after LogBuffer/LogTee setup so
// even a startup crash gets a marker written and can still pull recent log
// lines. log_buffer may be nullptr if a service doesn't want the recent-
// lines dump (onTerminate only — the signal handler never touches it).
inline void installCrashHandlers(const std::string& service_name, const std::string& data_dir,
								  LogBuffer* log_buffer = nullptr) {
	crash_handler::serviceNameRef() = service_name;
	crash_handler::dataDirRef() = data_dir;
	crash_handler::logBufferRef() = log_buffer;
	std::set_terminate(crash_handler::onTerminate);
	::signal(SIGSEGV, crash_handler::onFatalSignal);
	::signal(SIGABRT, crash_handler::onFatalSignal);
	::signal(SIGFPE,  crash_handler::onFatalSignal);
	::signal(SIGILL,  crash_handler::onFatalSignal);
	::signal(SIGBUS,  crash_handler::onFatalSignal);
}

// Reads back an existing crash marker (if any) for this service — used by
// the activity/debugging endpoint. Returns empty string if none exists.
// Deliberately does NOT delete the marker on read — an admin should be able
// to reload the page and still see the last crash until they explicitly
// acknowledge/clear it via clearCrashMarker() below.
inline std::string readCrashMarker(const std::string& data_dir, const std::string& service_name) {
	std::string path = data_dir + "/crash-" + service_name + ".txt";
	FILE* f = fopen(path.c_str(), "r");
	if (!f) return "";
	std::string out;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
	fclose(f);
	return out;
}

// The explicit "I've seen this" acknowledgment readCrashMarker's own
// comment always pointed at — an admin clears the marker once they've dealt
// with (or confirmed non-issue) a crash, rather than it just silently
// persisting until the next unrelated crash happens to overwrite it. A
// missing file is not an error (already cleared, or never crashed) —
// removing it either way is the whole point, so both cases return true.
inline bool clearCrashMarker(const std::string& data_dir, const std::string& service_name) {
	std::string path = data_dir + "/crash-" + service_name + ".txt";
	if (std::remove(path.c_str()) == 0) return true;
	return errno == ENOENT;
}
