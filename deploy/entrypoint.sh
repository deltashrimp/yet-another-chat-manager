#!/bin/sh
# Container entrypoint: make sure the /data volume is writable, then hand
# the process over to the bot as the unprivileged "yacm" user. `exec`
# replaces this shell, so the bot becomes PID 1 and receives SIGTERM
# directly (graceful shutdown: drain queue, close DB with WAL checkpoint).
# setpriv comes from util-linux, present in every Debian slim base.
set -e

mkdir -p /data
# Recursive chown: a bind-mounted ./data may already contain DB files
# created by a host-side run or an earlier container (local default
# DB_PATH=./data/bot.db). chowning only the directory leaves those files
# owned by another uid and the unprivileged bot hits SQLITE_READONLY
# ("attempt to write a readonly database"). The whole tree is owned by
# yacm so the bot can write the DB and its WAL/SHM sidecars.
chown -R yacm:yacm /data

exec setpriv --reuid=yacm --regid=yacm --init-groups /app/bot "$@"
