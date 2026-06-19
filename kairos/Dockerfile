# Stage 1: build
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build g++ git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DFETCHCONTENT_QUIET=OFF \
    && cmake --build build --parallel

# Stage 2: runtime
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    ffmpeg \
    curl \
    ca-certificates \
    unzip \
    && rm -rf /var/lib/apt/lists/* \
    && curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux \
       -o /usr/local/bin/yt-dlp \
    && chmod a+rx /usr/local/bin/yt-dlp \
    && curl -fsSL https://github.com/denoland/deno/releases/latest/download/deno-x86_64-unknown-linux-gnu.zip \
       -o /tmp/deno.zip \
    && unzip /tmp/deno.zip -d /usr/local/bin/ \
    && rm /tmp/deno.zip \
    && chmod a+rx /usr/local/bin/deno

COPY --from=builder /src/build/kairos /usr/local/bin/kairos
COPY --from=builder /src/ui-dist /app/ui-dist

# /app is the working directory so ./ui-dist resolves correctly.
# Config and database live in /data (the mounted volume) and are
# referenced by explicit absolute paths.
WORKDIR /app
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/kairos"]
CMD ["--db", "/data/kairos.db", "--conf", "/data/kairos.conf", "--port", "8080"]
