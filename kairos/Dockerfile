# Stage 1: build
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build g++ git ca-certificates \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy only the build manifest first. CMake configure fetches all FetchContent
# dependencies here — this layer is cached until CMakeLists.txt changes.
COPY CMakeLists.txt .
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DFETCHCONTENT_QUIET=OFF

# Source changes only rebuild from this point.
COPY src/ src/
RUN cmake --build build --parallel

# Stage 2: runtime
FROM ubuntu:24.04

# Set by Buildx; used to pick the correct arch-specific binaries below.
ARG TARGETARCH

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    libssl3 \
    ffmpeg \
    curl \
    ca-certificates \
    unzip \
    && rm -rf /var/lib/apt/lists/* \
    && YTDLP_SUFFIX=$([ "$TARGETARCH" = "arm64" ] && echo "_aarch64" || echo "") \
    && curl -fsSL "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux${YTDLP_SUFFIX}" \
       -o /usr/local/bin/yt-dlp \
    && chmod a+rx /usr/local/bin/yt-dlp \
    && DENO_ARCH=$([ "$TARGETARCH" = "arm64" ] && echo "aarch64" || echo "x86_64") \
    && curl -fsSL "https://github.com/denoland/deno/releases/latest/download/deno-${DENO_ARCH}-unknown-linux-gnu.zip" \
       -o /tmp/deno.zip \
    && unzip /tmp/deno.zip -d /usr/local/bin/ \
    && rm /tmp/deno.zip \
    && chmod a+rx /usr/local/bin/deno

COPY --from=builder /src/build/kairos /usr/local/bin/kairos

WORKDIR /app
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/kairos"]
CMD ["--db", "/data/kairos.db", "--conf", "/data/kairos.conf", "--port", "8080"]
