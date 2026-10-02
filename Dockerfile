# Docker is for the headless core/CLI demo. The native Qt desktop UI is built separately on each platform.
FROM debian:13-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake libsqlite3-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DBUILD_GUI=OFF -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

FROM debian:13-slim
RUN apt-get update && apt-get install -y --no-install-recommends libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/build/everything-lite-cli /usr/local/bin/everything-lite-cli
ENV EVERYTHING_LITE_DB=/data/everything-lite.db
VOLUME ["/data"]
WORKDIR /search
ENTRYPOINT ["/usr/local/bin/everything-lite-cli"]
CMD ["stats"]
