#include "GpuMetrics.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace {

// Same popen-based subprocess-capture approach the rest of this codebase
// already uses for ffprobe — a missing binary or non-zero exit just yields
// an empty string, which the caller treats as "query failed."
std::string runCommand(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen((cmd + " 2>/dev/null").c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    return result;
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) {
        size_t start = field.find_first_not_of(" \t\r\n");
        size_t end   = field.find_last_not_of(" \t\r\n");
        out.push_back(start == std::string::npos ? "" : field.substr(start, end - start + 1));
    }
    return out;
}

std::optional<GpuMetrics> queryNvidia() {
    std::string out = runCommand(
        "nvidia-smi --query-gpu=name,utilization.gpu,utilization.encoder,utilization.decoder,"
        "memory.used,memory.total,temperature.gpu --format=csv,noheader,nounits");
    if (out.empty()) return std::nullopt;

    // Only the first GPU — multi-GPU boxes aren't a scenario this app's
    // single process-wide hw_accel setting supports anyway (see
    // HwCapabilities' own "resolved once for the process's entire
    // lifetime" comment in HwProbe.h).
    auto fields = splitCsv(out.substr(0, out.find('\n')));
    if (fields.size() < 7) return std::nullopt;

    try {
        GpuMetrics m;
        m.backend           = "nvidia";
        m.name               = fields[0];
        m.gpu_util_pct       = std::stod(fields[1]);
        m.encoder_util_pct   = std::stod(fields[2]);
        m.decoder_util_pct   = std::stod(fields[3]);
        m.mem_used_mb        = std::stol(fields[4]);
        m.mem_total_mb       = std::stol(fields[5]);
        m.temp_c             = std::stod(fields[6]);
        return m;
    } catch (...) {
        return std::nullopt;
    }
}

// amdgpu's own sysfs interface — no external tool dependency (radeontop
// isn't part of the container image, and there's no NVML-equivalent
// library already linked for AMD the way this file shells out to
// nvidia-smi for NVIDIA). There's no standard exposure of separate
// encode/decode (VCN) engine utilization the way nvidia-smi has, so this
// backend only ever reports overall gpu_util_pct/memory/temp — see
// GpuMetrics.h's own comment on why encoder_util_pct/decoder_util_pct stay
// unset here.
std::optional<GpuMetrics> queryAmd(const std::string& vaapi_device) {
    namespace fs = std::filesystem;
    std::string renderNode = fs::path(vaapi_device).filename().string(); // e.g. "renderD128"
    if (renderNode.empty()) return std::nullopt;

    fs::path devDir = fs::path("/sys/class/drm") / renderNode / "device";
    std::error_code ec;
    if (!fs::exists(devDir, ec)) return std::nullopt;

    auto readLong = [&](const char* file) -> std::optional<long> {
        std::ifstream f(devDir / file);
        long v;
        if (f >> v) return v;
        return std::nullopt;
    };

    // gpu_busy_percent is the one field every amdgpu-driven card exposes —
    // treat its absence as "this isn't actually an amdgpu render node" and
    // bail rather than returning a mostly-empty GpuMetrics.
    auto busy = readLong("gpu_busy_percent");
    if (!busy) return std::nullopt;

    GpuMetrics m;
    m.backend = "amd";
    m.gpu_util_pct = static_cast<double>(*busy);
    if (auto used = readLong("mem_info_vram_used"))   m.mem_used_mb  = *used  / (1024 * 1024);
    if (auto total = readLong("mem_info_vram_total")) m.mem_total_mb = *total / (1024 * 1024);

    // hwmon's subdirectory name (hwmon0, hwmon1, ...) isn't fixed, so scan
    // for it rather than assuming a number — best-effort, temp_c just stays
    // unset if no hwmon entry is found.
    for (auto& entry : fs::directory_iterator(devDir / "hwmon", ec)) {
        std::ifstream t(entry.path() / "temp1_input");
        long milli;
        if (t >> milli) { m.temp_c = milli / 1000.0; break; }
    }
    return m;
}

} // namespace

std::optional<GpuMetrics> queryGpuMetrics(HwAccel backend, const std::string& vaapi_device) {
    switch (backend) {
        case HwAccel::nvidia: return queryNvidia();
        case HwAccel::amd:    return queryAmd(vaapi_device);
        default:              return std::nullopt;
    }
}
