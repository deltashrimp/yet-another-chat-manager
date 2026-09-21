#!/bin/sh
# Container entrypoint: make sure the /data volume is writable, then hand
# the process over to the bot as the unprivileged "yacm" user. `exec`
# replaces this shell, so the bot becomes PID 1 and receives SIGTERM
# directly (graceful shutdown: drain queue, close DB with WAL checkpoint).
# setpriv comes from util-linux, present in every Debian slim base.
set -e

mkdir -p /data
chown yacm:yacm /data

exec setpriv --reuid=yacm --regid=yacm --init-groups /app/bot "$@"