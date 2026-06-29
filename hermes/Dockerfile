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
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/hermes /usr/local/bin/hermes

EXPOSE 8000
ENTRYPOINT ["/usr/local/bin/hermes"]
CMD ["--port", "8000"]
