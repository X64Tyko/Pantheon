#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct DownloadJob
{
	std::string id;
	std::string url;
	std::string dest_path;
	std::string status;          // "queued" | "running" | "done" | "error"
	int progress;                // 0–100
	int playlist_index = 0;      // current item number within a playlist (0 = not a playlist / unknown)
	int playlist_count = 0;      // total items in the playlist (0 = not a playlist / unknown)
	std::deque<std::string> log; // last 100 lines of yt-dlp output
	std::string started_at;      // ISO-8601 UTC
};

class DownloadManager
{
public:
	// Spawn yt-dlp for the given URL and return the job ID.
	std::string startJob(const std::string& url, const std::string& dest_path);

	// Snapshot of all jobs, newest first.
	std::vector<DownloadJob> getJobs() const;

private:
	void runJob(std::string id);

	std::deque<DownloadJob>::iterator findJob(const std::string& id);

	mutable std::mutex mu_;
	std::deque<DownloadJob> jobs_; // oldest at front, capped at 50
};