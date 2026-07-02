#include "HwProbe.h"
#include "EncoderArgs.h"
#include <array>
#include <cstdio>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Matches the encode probe's nullsrc resolution AND the Dockerfile's decode
// sample resolution. NVDEC enforces a per-codec minimum decode width (144px
// for hevc, 128px for av1, confirmed empirically against this driver/build)
// — anything smaller either hard-fails the decode probe or, worse, silently
// falls back to software decode while still exiting 0. 256x144 clears both.
constexpr int kProbeWidth  = 256;
constexpr int kProbeHeight = 144;

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    out += "'";
    return out;
}

std::string shellJoin(const std::vector<std::string>& args) {
    std::string cmd;
    for (size_t i = 0; i < args.size(); ++i) { if (i) cmd += ' '; cmd += shellQuote(args[i]); }
    return cmd;
}

// ffmpeg can fail hwaccel setup and still silently fall back to software
// decode, exiting 0 — confirmed directly: a too-small NVDEC decode probe
// printed "Failed setup for format cuda: hwaccel initialisation returned
// error" yet still returned exit code 0. Exit code alone is not trustworthy
// for these probes; the captured output has to be scanned too.
bool mentionsHwaccelFailure(const std::string& output) {
    static const char* kSignatures[] = {
        "hwaccel initialisation returned error",
        "Failed setup for format",
        "No usable encoding entrypoint found",
        "Function not implemented",
        "Decode error rate",
    };
    for (auto* sig : kSignatures) if (output.find(sig) != std::string::npos) return true;
    return false;
}

struct ProbeResult { bool ok; std::string output; };

// Synchronous/blocking by design — this only ever runs a handful of times at
// startup, before the HTTP server or any session machinery exists, so
// there's no need for FfmpegProcess's async fork/exec/waitpid-with-callbacks
// machinery. Mirrors MediaProbe.cpp's existing popen()/pclose() pattern for
// the same reason (a one-shot blocking subprocess call).
ProbeResult runProbe(const std::vector<std::string>& args) {
    std::string cmd = shellJoin(args) + " 2>&1";
    std::array<char, 4096> buf{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {false, ""};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) output += buf.data();
    int status = pclose(pipe);
    bool exited_ok = status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return {exited_ok && !mentionsHwaccelFailure(output), output};
}

bool deviceNodeAccessible(HwAccel backend, const std::string& vaapi_device) {
    switch (backend) {
        case HwAccel::nvidia:
            if (access("/dev/nvidiactl", R_OK | W_OK) == 0) return true;
            return access("/dev/nvidia0", R_OK | W_OK) == 0;
        case HwAccel::amd:
            return access(vaapi_device.c_str(), R_OK | W_OK) == 0;
        default:
            return false;
    }
}

// h264-only, matching pushVideoEncoderArgs' own scope: it only ever outputs
// h264 (libx264/h264_nvenc/h264_vaapi), there is no HEVC/AV1 encode path in
// this codebase, so testing hevc_nvenc/av1_nvenc encode would validate a
// capability nothing uses.
std::vector<std::string> buildEncodeProbeArgs(HwAccel backend, const std::string& ffmpeg_path,
                                               const std::string& vaapi_device) {
    std::vector<std::string> a{ffmpeg_path, "-v", "error"};
    pushVaapiDeviceArg(a, backend, HwAccel::none, vaapi_device); // pre-input, no-op unless amd
    a.insert(a.end(), {"-f", "lavfi", "-i",
                        "nullsrc=s=" + std::to_string(kProbeWidth) + "x" +
                        std::to_string(kProbeHeight) + ":d=0.1"});
    std::vector<std::string> vfParts;
    pushVideoEncoderArgs(a, vfParts, backend, /*keyframeIntervalSecs=*/2); // exact same encoder selection real sessions use
    pushVideoFilterArgs(a, vfParts);
    a.insert(a.end(), {"-frames:v", "2", "-f", "null", "-"});
    return a;
}

// Deliberately not routed through the gated pushHwAccelDecodeArgs -- that
// function's whole job is to consult decodable_codecs, which is the very set
// this probe is computing (chicken-and-egg). Just the raw flag.
std::vector<std::string> buildDecodeProbeArgs(HwAccel backend, const std::string& ffmpeg_path,
                                               const std::string& vaapi_device,
                                               const std::string& sample_path) {
    std::vector<std::string> a{ffmpeg_path, "-v", "error"};
    pushVaapiDeviceArg(a, HwAccel::none, backend, vaapi_device); // no-op unless amd
    if (backend == HwAccel::nvidia) a.insert(a.end(), {"-hwaccel", "cuda"});
    else if (backend == HwAccel::amd) a.insert(a.end(), {"-hwaccel", "vaapi"});
    a.insert(a.end(), {"-i", sample_path, "-frames:v", "1", "-f", "null", "-"});
    return a;
}

} // namespace

HwCapabilities probeHwCapabilities(HwAccel requested, const std::string& ffmpeg_path,
                                    const std::string& vaapi_device, const std::string& assets_dir,
                                    bool verbose_transcode_logs) {
    HwCapabilities caps;
    if (requested == HwAccel::none) return caps;

    if (!deviceNodeAccessible(requested, vaapi_device)) {
        std::cerr << "[hwprobe] " << hwAccelName(requested)
                  << " device node not accessible, falling back to software encode/decode\n";
        return caps;
    }

    auto encodeResult = runProbe(buildEncodeProbeArgs(requested, ffmpeg_path, vaapi_device));
    if (encodeResult.ok) {
        caps.encode = requested;
        std::cout << "[hwprobe] " << hwAccelName(requested) << " encode: OK\n";
    } else {
        std::cerr << "[hwprobe] " << hwAccelName(requested)
                  << " h264 encode smoke-test failed, falling back to software encode\n"
                  << encodeResult.output;
    }

    // Decode is probed independently of the encode result above -- a session
    // can legitimately end up GPU-decode+CPU-encode (e.g. an NVENC session
    // cap hit while NVDEC is still free) or the reverse (AMD's existing
    // default). See HwCapabilities's own comments for the full rationale.
    caps.decode = requested;
    // 10-bit hevc/av1 get their own samples: hardware decode support varies
    // by bit depth (older NVDEC/VAAPI generations often handle 8-bit HEVC
    // but not 10-bit), and 10-bit is close to universal for modern HEVC/AV1
    // media libraries (mandatory for HDR, common even for SDR since it
    // compresses better) -- an 8-bit-only probe would give a false
    // "decodable" signal for the 10-bit files that actually dominate. h264's
    // 10-bit (High 10) profile is rare enough in practice not to bother.
    static const std::tuple<const char*, int, const char*> kSamples[] = {
        {"h264", 8,  "probe_h264.mp4"},
        {"hevc", 8,  "probe_hevc.mp4"},
        {"hevc", 10, "probe_hevc_10bit.mp4"},
        {"av1",  8,  "probe_av1.mp4"},
        {"av1",  10, "probe_av1_10bit.mp4"},
    };
    for (auto& [codec, bit_depth, filename] : kSamples) {
        std::string path = assets_dir + "/" + filename;
        std::string key = decodeCodecKey(codec, bit_depth);
        if (access(path.c_str(), R_OK) != 0) {
            std::cerr << "[hwprobe] decode sample missing: " << path
                      << " (skipping " << key << " decode probe)\n";
            continue;
        }
        bool ok = runProbe(buildDecodeProbeArgs(requested, ffmpeg_path, vaapi_device, path)).ok;
        std::cout << "[hwprobe] " << hwAccelName(requested) << " decode " << key << ": "
                  << (ok ? "OK" : "FAILED") << "\n";
        if (ok) caps.decodable_codecs.insert(key);
    }

    if (verbose_transcode_logs) {
        std::cout << "[hwprobe] final supported combinations for " << hwAccelName(requested) << ":\n";
        std::cout << "[hwprobe]   encode: "
                  << (caps.encode != HwAccel::none ? "h264" : "none (software fallback)") << "\n";
        std::cout << "[hwprobe]   decode: ";
        if (caps.decodable_codecs.empty()) {
            std::cout << "none (software fallback)";
        } else {
            bool first = true;
            for (auto& key : caps.decodable_codecs) {
                if (!first) std::cout << ", ";
                std::cout << key;
                first = false;
            }
        }
        std::cout << "\n";
    }

    return caps;
}
