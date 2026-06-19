#include "DownloadManager.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

namespace {

std::string generateId() {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << dist(rng);
    return ss.str();
}

std::string nowIso() {
    auto now  = std::chrono::system_clock::now();
    std::time_t t  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// Wrap s in single quotes, escaping any interior single quotes.
std::string shellQuote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else           r += c;
    }
    return r + "'";
}

// Parse a percentage from a yt-dlp progress line; returns -1 if not found.
int parseProgress(const std::string& line) {
    auto pct = line.find('%');
    if (pct == std::string::npos) return -1;
    size_t start = pct;
    while (start > 0 && (std::isdigit(static_cast<unsigned char>(line[start - 1]))
                         || line[start - 1] == '.'))
        --start;
    if (start == pct) return -1;
    try {
        return static_cast<int>(std::stof(line.substr(start, pct - start)));
    } catch (...) {
        return -1;
    }
}

} // namespace

// ---------------------------------------------------------------------------

std::string DownloadManager::startJob(const std::string& url,
                                       const std::string& dest_path) {
    std::string id = generateId();
    {
        std::lock_guard lk(mu_);
        // Prune the oldest finished job if at capacity.
        if (jobs_.size() >= 50) {
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                if (it->status == "done" || it->status == "error") {
                    jobs_.erase(it);
                    break;
                }
            }
        }
        DownloadJob job;
        job.id         = id;
        job.url        = url;
        job.dest_path  = dest_path;
        job.status     = "queued";
        job.progress   = 0;
        job.started_at = nowIso();
        jobs_.push_back(std::move(job));
    }
    std::thread([this, id]{ runJob(id); }).detach();
    return id;
}

std::vector<DownloadJob> DownloadManager::getJobs() const {
    std::lock_guard lk(mu_);
    return std::vector<DownloadJob>(jobs_.rbegin(), jobs_.rend());
}

std::deque<DownloadJob>::iterator DownloadManager::findJob(const std::string& id) {
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
        if (it->id == id) return it;
    return jobs_.end();
}

void DownloadManager::runJob(std::string id) {
    std::string url, dest;
    {
        std::lock_guard lk(mu_);
        auto it = findJob(id);
        if (it == jobs_.end()) return;
        it->status = "running";
        url  = it->url;
        dest = it->dest_path;
    }

    std::string cmd = "yt-dlp --newline --paths " + shellQuote(dest) +
                      " --output '%(title)s.%(ext)s' " + shellQuote(url) + " 2>&1";

    std::cout << "[download] job " << id << " starting: " << url << "\n";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::lock_guard lk(mu_);
        auto it = findJob(id);
        if (it != jobs_.end()) {
            it->status = "error";
            it->log.push_back("Failed to start yt-dlp — is it installed?");
        }
        return;
    }

    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty()) continue;

        int pct = parseProgress(line);

        std::lock_guard lk(mu_);
        auto it = findJob(id);
        if (it == jobs_.end()) break;
        if (pct >= 0) it->progress = pct;
        it->log.push_back(std::move(line));
        if (it->log.size() > 100) it->log.pop_front();
    }

    int ret = pclose(pipe);

    std::lock_guard lk(mu_);
    auto it = findJob(id);
    if (it != jobs_.end()) {
        it->status   = (ret == 0) ? "done" : "error";
        if (ret == 0) it->progress = 100;
    }
    std::cout << "[download] job " << id
              << (ret == 0 ? " done\n" : " failed (exit " + std::to_string(ret) + ")\n");
}
