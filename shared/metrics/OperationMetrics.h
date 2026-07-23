#pragma once
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include "thread/TaskRegistry.h" // stopTokenSleep

// Structured per-operation metrics (time/CPU/RAM/threads), one level up from
// MetricsGatherer's whole-process point-in-time gauge (kairos/src/util/
// MetricsGatherer.h). Shared by kairos/hermes/hephaestus per the shared/
// convention (see thread/TaskRegistry.h, log/LogBuffer.h) — v1 only wires up
// Kairos's EPG/sync hot zones, but nothing here is Kairos-specific.

struct OperationRun {
    int64_t started_at_ms  = 0;
    int64_t duration_ms    = 0;
    double  avg_cpu_pct    = 0.0;
    double  max_cpu_pct    = 0.0;
    long    avg_ram_bytes  = 0;
    long    max_ram_bytes  = 0;
    int     peak_threads   = 0;
    int     samples        = 0;
};

// Bounded per-name run history. In-memory only, resets on restart — same
// non-persistence as every other metric in this system (MetricsStore.ts's
// client-side ring buffer, MetricsGatherer's static delta state).
class OperationMetricsStore {
public:
    static OperationMetricsStore& global() {
        static OperationMetricsStore inst;
        return inst;
    }

    void record(const std::string& name, OperationRun run) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto& q = runs_[name];
        q.push_front(std::move(run));
        while (q.size() > kMaxPerOp) q.pop_back();
    }

    // Most-recent-first per operation name.
    std::map<std::string, std::vector<OperationRun>> snapshot() const {
        std::lock_guard<std::mutex> lock(mtx_);
        std::map<std::string, std::vector<OperationRun>> out;
        for (auto& [name, q] : runs_) out[name] = std::vector<OperationRun>(q.begin(), q.end());
        return out;
    }

private:
    OperationMetricsStore() = default;
    static constexpr size_t kMaxPerOp = 20;
    mutable std::mutex mtx_;
    std::unordered_map<std::string, std::deque<OperationRun>> runs_;
};

// RAII — construct at the top of the scope being measured, destruct records
// the finished run. Owns its own dedicated sampling thread (~200ms default)
// so long operations (a multi-minute sync phase) get real avg/max CPU/RAM/
// thread readings across their whole run, not just a start/end snapshot.
//
// Deliberately does NOT share MetricsGatherer's static CPU-delta state
// (kairos/src/util/MetricsGatherer.h) — that state is already read/written
// from whichever thread handles /api/system/metrics on its own 1s cadence;
// a second concurrent sampler reusing the same statics would race. Each
// instance keeps its own independent utime/stime/total_time baseline
// instead, at zero extra cost.
//
// Only wrap genuinely hot/expensive call sites with this — see its call
// sites for the "why here, not there" reasoning (e.g. EPGMaterializer's
// ensureScheduled() only wraps the actual regenerate branch, not the whole
// function, since that function is hit on every /now poll).
class OperationRecorder {
public:
    explicit OperationRecorder(std::string name, int sample_interval_ms = 200)
        : name_(std::move(name)), start_(std::chrono::steady_clock::now()) {
        primeCpuBaseline();
        sampleOnce(); // guarantees a real first reading even if this op finishes before the first tick
        sampler_ = std::jthread([this, sample_interval_ms](std::stop_token st) {
            while (!st.stop_requested()) {
                stopTokenSleep(st, std::chrono::milliseconds(sample_interval_ms));
                if (st.stop_requested()) break;
                sampleOnce();
            }
        });
    }

    ~OperationRecorder() {
        sampler_.request_stop();
        if (sampler_.joinable()) sampler_.join();

        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count();
        const auto started_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() - duration;

        std::lock_guard<std::mutex> lock(stats_mtx_);
        OperationRun run;
        run.started_at_ms = started_at_ms;
        run.duration_ms   = duration;
        run.avg_cpu_pct   = cpu_n_  > 0 ? cpu_sum_ / cpu_n_ : 0.0;
        run.max_cpu_pct   = cpu_max_;
        run.avg_ram_bytes = ram_n_  > 0 ? ram_sum_ / ram_n_ : 0;
        run.max_ram_bytes = ram_max_;
        run.peak_threads  = threads_max_;
        run.samples       = static_cast<int>(cpu_n_);
        OperationMetricsStore::global().record(name_, std::move(run));
    }

    OperationRecorder(const OperationRecorder&)            = delete;
    OperationRecorder& operator=(const OperationRecorder&) = delete;

private:
    // First /proc/self/stat + /proc/stat read establishes this instance's own
    // delta baseline — without it the very first sampleOnce() call would
    // compute a bogus 100%-of-nothing delta the way MetricsGatherer's first
    // call after process start always reports 0.
    void primeCpuBaseline() {
        readCpuTicks(last_utime_, last_stime_, last_total_time_);
    }

    static void readCpuTicks(long& utime, long& stime, long& total_time) {
        std::ifstream stat("/proc/self/stat");
        std::string dummy;
        for (int i = 0; i < 13; ++i) stat >> dummy;
        stat >> utime >> stime;

        std::ifstream sys_stat("/proc/stat");
        std::string cpu;
        sys_stat >> cpu;
        long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
        sys_stat >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;
        total_time = user + nice + system + idle + iowait + irq + softirq + steal;
    }

    static long readRamBytes() {
        std::ifstream statm("/proc/self/statm");
        long rss_pages = 0;
        if (statm >> rss_pages >> rss_pages) return rss_pages * sysconf(_SC_PAGESIZE);
        return 0;
    }

    static int readThreadCount() {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("Threads:", 0) == 0) {
                try { return std::stoi(line.substr(8)); } catch (...) { return 0; }
            }
        }
        return 0;
    }

    void sampleOnce() {
        long utime, stime, total_time;
        readCpuTicks(utime, stime, total_time);
        double cpu = 0.0;
        const long total_delta = total_time - last_total_time_;
        if (total_delta > 0) {
            const long proc_delta = (utime + stime) - (last_utime_ + last_stime_);
            cpu = 100.0 * proc_delta / total_delta;
        }
        last_utime_ = utime; last_stime_ = stime; last_total_time_ = total_time;

        const long ram = readRamBytes();
        const int  threads = readThreadCount();

        std::lock_guard<std::mutex> lock(stats_mtx_);
        cpu_sum_ += cpu; cpu_n_++; if (cpu > cpu_max_) cpu_max_ = cpu;
        ram_sum_ += ram; ram_n_++; if (ram > ram_max_) ram_max_ = ram;
        if (threads > threads_max_) threads_max_ = threads;
    }

    std::string name_;
    std::chrono::steady_clock::time_point start_;
    std::jthread sampler_;

    std::mutex stats_mtx_;
    double cpu_sum_ = 0.0; long cpu_n_ = 0; double cpu_max_ = 0.0;
    long   ram_sum_ = 0;   long ram_n_ = 0; long   ram_max_ = 0;
    int    threads_max_ = 0;

    long last_utime_ = 0, last_stime_ = 0, last_total_time_ = 0;
};
