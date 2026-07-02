# Stage 1: build
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build g++ git ca-certificates \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt .
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DFETCHCONTENT_QUIET=OFF

COPY src/ src/
RUN cmake --build build --parallel

# Stage 2: runtime
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    libssl3 \
    ffmpeg \
    libva-drm2 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/hephaestus /usr/local/bin/hephaestus
COPY assets/default_logo.png /usr/local/share/hephaestus/assets/default_logo.png

# Tiny sample clips for HwProbe's decode smoke-test (see
# src/stream/HwProbe.cpp) -- one per codec that matters for a movie/TV media
# library (h264/hevc/av1; vp9 deliberately skipped, it's mostly a web/YouTube
# codec). Generated here via software encode so they always match this
# image's own ffmpeg build, rather than committed as binaries.
#
# 256x144, not smaller: NVDEC enforces a per-codec minimum decode width
# (144px for hevc, 128px for av1, confirmed empirically) -- anything smaller
# either hard-fails the probe or, worse, silently falls back to software
# decode while still exiting 0. This also matches HwProbe's own encode-probe
# nullsrc resolution.
#
# Each generation is independently allowed to fail (e.g. a stripped-down
# ffmpeg build missing libsvtav1) -- HwProbe.cpp already skips a codec's
# decode probe entirely when its asset is missing, so a missing sample here
# degrades gracefully at runtime instead of failing the image build.
RUN mkdir -p /usr/local/share/hephaestus/assets && \
    (ffmpeg -y -v error -f lavfi -i testsrc=size=256x144:rate=5:duration=1 \
        -c:v libx264 -pix_fmt yuv420p \
        /usr/local/share/hephaestus/assets/probe_h264.mp4 \
        || echo "[docker] probe_h264.mp4 generation failed, HwProbe will skip h264 decode probe") && \
    (ffmpeg -y -v error -f lavfi -i testsrc=size=256x144:rate=5:duration=1 \
        -c:v libx265 -pix_fmt yuv420p -tag:v hvc1 \
        /usr/local/share/hephaestus/assets/probe_hevc.mp4 \
        || echo "[docker] probe_hevc.mp4 generation failed, HwProbe will skip hevc decode probe") && \
    (ffmpeg -y -v error -f lavfi -i testsrc=size=256x144:rate=5:duration=1 \
        -c:v libsvtav1 -pix_fmt yuv420p \
        /usr/local/share/hephaestus/assets/probe_av1.mp4 \
        || echo "[docker] probe_av1.mp4 generation failed, HwProbe will skip av1 decode probe")

EXPOSE 8082
ENTRYPOINT ["/usr/local/bin/hephaestus"]
CMD ["--port", "8082"]
