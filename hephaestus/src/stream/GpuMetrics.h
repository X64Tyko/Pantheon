#pragma once
#include "ChannelSession.h" // HwAccel
#include <optional>
#include <string>

// Live GPU stats for the Activity page's system-resources section — a
// different concern from HwCapabilities (which is "can this backend decode/
// encode at all," resolved once at startup): this is "how busy is it right
// now," queried fresh on every request the same way MetricsGatherer already
// does for process CPU/RAM.
struct GpuMetrics {
    std::string backend; // "nvidia" | "amd"
    std::optional<std::string> name;
    double gpu_util_pct = 0;
    // NVIDIA only — nvidia-smi exposes separate NVENC/NVDEC engine
    // utilization; there's no AMD equivalent already wired up (no NVML-
    // style library linked for AMD, and VAAPI has no standard utilization
    // query), so these stay unset for backend == "amd" and the Activity
    // page renders accordingly.
    std::optional<double> encoder_util_pct;
    std::optional<double> decoder_util_pct;
    long mem_used_mb = 0;
    long mem_total_mb = 0;
    std::optional<double> temp_c;
};

// Returns nullopt when backend == HwAccel::none, or when the query itself
// fails (nvidia-smi missing/erroring, amdgpu sysfs nodes absent) — callers
// should omit the "gpu" key entirely in that case, and the Activity page
// simply doesn't draw a GPU section, same "degrade to absent, not a hard
// failure" convention HwProbe's own sample-probing already uses.
std::optional<GpuMetrics> queryGpuMetrics(HwAccel backend, const std::string& vaapi_device);
