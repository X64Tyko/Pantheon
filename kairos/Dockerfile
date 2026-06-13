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
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/kairos /usr/local/bin/kairos

EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/kairos"]
CMD ["--db", "/data/kairos.db", "--port", "8080"]
