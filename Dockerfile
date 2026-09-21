# syntax=docker/dockerfile:1
#
# Multi-stage build for a single-container deployment (spec §Deploy):
#   builder  — full toolchain + dev headers; FetchContent pulls asio and
#              nlohmann/json at configure time (network needed only here).
#   runtime  — minimal Debian bookworm-slim with just the runtime libs,
#              the binary, translations and the welcome message.
#
# Build:   docker build -t yacm .
# Run:     docker compose up -d   (see docker-compose.yml)

# ---- build stage ---------------------------------------------------------
FROM debian:bookworm-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        libcurl4-openssl-dev \
        libfmt-dev \
        libspdlog-dev \
        libsqlite3-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Tests run on the host via ctest; the image only needs the release binary.
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DYACM_BUILD_TESTS=OFF \
    && cmake --build build -j"$(nproc)"

# ---- runtime stage -------------------------------------------------------
FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libcurl4 \
        libfmt9 \
        libspdlog1.10 \
        libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 yacm

WORKDIR /app
COPY --from=builder /src/build/bot /app/bot
COPY --from=builder /src/i18n /app/i18n
COPY --from=builder /src/resources /app/resources
COPY --from=builder /src/deploy/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# Volume-mounted SQLite database (+ WAL/SHM). docker-compose bind-mounts
# ./data to /data and overrides the local ./.env paths via `environment:`.
ENV DB_PATH=/data/bot.db \
    I18N_DIR=/app/i18n \
    WELCOME_MESSAGE=/app/resources/welcome-message.md \
    DEFAULT_LANG=en \
    LOG_LEVEL=info \
    POLL_TIMEOUT=30

VOLUME /data

# Entrypoint prepares /data, then `exec`s the bot as the unprivileged "yacm"
# user, so PID 1 is the bot and SIGTERM (docker stop) hits it directly for
# graceful shutdown (drain queue, WAL checkpoint).
ENTRYPOINT ["/app/entrypoint.sh"]