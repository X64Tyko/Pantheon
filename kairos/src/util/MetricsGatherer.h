#pragma once
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

struct ProcessMetrics {
    double cpu_usage = 0.0; // Percentage (0-100)
    long ram_bytes = 0;
};

struct SystemMetrics {
    double total_cpu_usage = 0.0;
    long total_ram_bytes = 0;
    long free_ram_bytes = 0;
};

class MetricsGatherer {
public:
    static ProcessMetrics getProcessMetrics() {
        ProcessMetrics m;
        
        // RAM from /proc/self/statm (RSS is the 2nd field in pages)
        std::ifstream statm("/proc/self/statm");
        long rss_pages = 0;
        if (statm >> rss_pages >> rss_pages) {
            m.ram_bytes = rss_pages * sysconf(_SC_PAGESIZE);
        }

        // CPU from /proc/self/stat
        // We need two samples to calculate percentage. 
        // For simplicity in a stateless API call, we'll just return raw ticks or a rough estimate
        // but better to just return the raw values and let the frontend or a background thread calculate delta.
        // Actually, let's keep it simple: we'll return total jiffies and the caller can delta.
        // Wait, the UI just wants a graph. Let's do the delta in a static local.
        
        static long last_utime = 0, last_stime = 0, last_total_time = 0;
        
        std::ifstream stat("/proc/self/stat");
        std::string dummy;
        for (int i = 0; i < 13; ++i) stat >> dummy;
        long utime, stime;
        stat >> utime >> stime;
        
        std::ifstream uptime("/proc/stat");
        std::string cpu;
        uptime >> cpu;
        long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
        uptime >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;
        long total_time = user + nice + system + idle + iowait + irq + softirq + steal;

        if (last_total_time > 0) {
            long total_delta = total_time - last_total_time;
            long proc_delta = (utime + stime) - (last_utime + last_stime);
            if (total_delta > 0) {
                m.cpu_usage = 100.0 * proc_delta / total_delta;
            }
        }
        
        last_utime = utime;
        last_stime = stime;
        last_total_time = total_time;

        return m;
    }

    static SystemMetrics getSystemMetrics() {
        SystemMetrics m;
        
        // RAM
        std::ifstream meminfo("/proc/meminfo");
        std::string label;
        long value;
        while (meminfo >> label >> value >> dummy_kb) {
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
        
        if (last_total > 0) {
            long total_delta = total - last_total;
            long idle_delta = idle - last_idle;
            if (total_delta > 0) {
                m.total_cpu_usage = 100.0 * (1.0 - (double)idle_delta / total_delta);
            }
        }
        
        last_idle = idle;
        last_total = total;

        return m;
    }

private:
    static inline std::string dummy_kb;
};
