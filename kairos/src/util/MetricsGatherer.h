#pragma once
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

struct ProcessMetrics
{
	double cpu_usage = 0.0; // Percentage (0-100)
	long ram_bytes   = 0;
};

struct SystemMetrics
{
	double total_cpu_usage = 0.0;
	long total_ram_bytes   = 0;
	long free_ram_bytes    = 0;
};

class MetricsGatherer
{
public:
	static ProcessMetrics getProcessMetrics()
	{
		ProcessMetrics m;

		// RAM: raw cgroup usage minus reclaimable page-cache.
		//
		// Originally subtracted only inactive_file to match what docker stats
		// itself reports — but that undercounts right after (or during) any
		// heavy sequential read, like the scene-detection/fingerprint scans
		// in detect/ChapterDetector.cpp: pages the kernel only just cached
		// are classified active_file until it gets around to aging them,
		// which can be many minutes on a mostly-idle host. Subtracting the
		// full file figure (all reclaimable page cache, active or not)
		// instead means a one-off library scan no longer makes this figure —
		// the one Kairos's own /api/activity endpoint hands to the Hades
		// Activity page — look like sustained app memory it isn't. This can
		// now read a bit lower than `docker stats`' own figure in that same
		// window; that's the more honest number, not a bug.
		{
			// Try cgroup v2
			std::ifstream f2("/sys/fs/cgroup/memory.current");
			long raw = 0;
			if (f2 >> raw)
			{
				long file_cache = 0;
				std::ifstream stat2("/sys/fs/cgroup/memory.stat");
				std::string key;
				long val;
				while (stat2 >> key >> val)
					if (key == "file")
					{
						file_cache = val;
						break;
					}
				m.ram_bytes = raw > file_cache ? raw - file_cache : 0;
			}
			else
			{
				// Try cgroup v1
				std::ifstream f1("/sys/fs/cgroup/memory/memory.usage_in_bytes");
				if (f1 >> raw)
				{
					long file_cache = 0;
					std::ifstream stat1("/sys/fs/cgroup/memory/memory.stat");
					std::string key;
					long val;
					while (stat1 >> key >> val)
						if (key == "total_cache")
						{
							file_cache = val;
							break;
						}
					m.ram_bytes = raw > file_cache ? raw - file_cache : 0;
				}
				else
				{
					// Fallback: /proc/self/statm RSS
					std::ifstream statm("/proc/self/statm");
					long dummy, rss;
					if (statm >> dummy >> rss) m.ram_bytes = rss * sysconf(_SC_PAGESIZE);
				}
			}
		}

		// CPU: expressed as a fraction of the service's thread capacity
		// (100% = all threads fully busy), matching the user's intuition and
		// preventing e.g. a 10-thread sync from showing 1000%. /proc/stat
		// totals span all host CPUs, so we first multiply by nproc (per-core
		// normalisation) then divide by the process's current thread count.
		static long last_utime = 0, last_stime = 0, last_total_time = 0;
		std::ifstream stat("/proc/self/stat");
		std::string dummy;
		for (int i = 0; i < 13; ++i) stat >> dummy;
		long utime, stime;
		stat >> utime >> stime;
		// Skip cutime, cstime, priority, nice (fields 16–19) to reach
		// num_threads (field 20).
		long skip4;
		stat >> skip4 >> skip4 >> skip4 >> skip4;
		long num_threads = 1;
		stat >> num_threads;
		if (num_threads < 1) num_threads = 1;

		std::ifstream uptime("/proc/stat");
		std::string cpu;
		uptime >> cpu;
		long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
		uptime >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;
		long total_time = user + nice + system + idle + iowait + irq + softirq + steal;

		if (last_total_time > 0)
		{
			long total_delta = total_time - last_total_time;
			long proc_delta  = (utime + stime) - (last_utime + last_stime);
			if (total_delta > 0)
			{
				int nproc   = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
				m.cpu_usage = 100.0 * proc_delta * nproc / total_delta / num_threads;
			}
		}

		last_utime      = utime;
		last_stime      = stime;
		last_total_time = total_time;

		return m;
	}

	static SystemMetrics getSystemMetrics()
	{
		SystemMetrics m;

		// RAM
		std::ifstream meminfo("/proc/meminfo");
		std::string label;
		long value;
		while (meminfo >> label >> value >> dummy_kb)
		{
			if (label == "MemTotal:") m.total_ram_bytes = value * 1024;
			else if (label == "MemAvailable:") m.free_ram_bytes = value * 1024;
		}

		// CPU
		static long last_idle = 0, last_total = 0;
		std::ifstream stat("/proc/stat");
		std::string cpu;
		stat >> cpu;
		long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
		stat >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;
		long total = user + nice + system + idle + iowait + irq + softirq + steal;

		if (last_total > 0)
		{
			long total_delta = total - last_total;
			long idle_delta  = idle - last_idle;
			if (total_delta > 0)
			{
				m.total_cpu_usage = 100.0 * (1.0 - (double)idle_delta / total_delta);
			}
		}

		last_idle  = idle;
		last_total = total;

		return m;
	}

private:
	static inline std::string dummy_kb;
};