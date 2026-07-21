# Stage 1: build
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build g++ git ca-certificates \
    libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Build context is the repo root (see docker-kairos.yml) so this stage can
# also reach shared/ — kairos/CMakeLists.txt and src/ are copied in below
# flattened to the same /src layout as before, so CMAKE_SOURCE_DIR still
# resolves shared/ the same way a monorepo build (dev.sh) does.
#
# Copy only the build manifest first. CMake configure fetches all FetchContent
# dependencies here — this layer is cached until CMakeLists.txt changes.
COPY kairos/CMakeLists.txt .
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DFETCHCONTENT_QUIET=OFF

# Source changes only rebuild from this point.
COPY shared/ shared/
COPY kairos/src/ src/
RUN cmake --build build --parallel

# Stage 2: runtime
FROM ubuntu:24.04

# Set by Buildx; used to pick the correct arch-specific binaries below.
ARG TARGETARCH

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    libssl3 \
    ffmpeg \
    libchromaprint-tools \
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

# Design tokens (see hades/scripts/generate-tv-tokens.mjs) — read at request
# time by TvManifestService, served as the manifest's "theme" key. Default
# path native clients' theme lives at; KAIROS_TV_TOKENS_PATH overrides it for
# local dev (see dev.sh/dev.ps1), same pattern as hephaestus's default_logo_path.
COPY kairos/assets/tv-tokens.json /usr/local/share/kairos/assets/tv-tokens.json

# No WORKDIR here (matches hermes/hephaestus) — main.cpp's log path is a
# relative "./data/kairos.log" default, and a WORKDIR of /app previously made
# it resolve to /app/data/kairos.log instead of the /data volume mount below,
# so the log silently never reached appdata while --db/--conf (passed as
# absolute paths in CMD) did.
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/kairos"]
CMD ["--db", "/data/kairos.db", "--conf", "/data/kairos.conf", "--port", "8080"]
