#!/bin/sh
# srv_test.sh — integration gauntlet for solsrv: boots the server on a scratch
# db and drives the real HTTP surface with curl. Grows with each task.
set -eu

PORT=18080
BASE="http://127.0.0.1:$PORT"
DB=srv_itest.db

rm -f "$DB" "$DB-wal" "$DB-shm"
./build.sh server >/dev/null

./solsrv -p 127.0.0.1:$PORT -d "$DB" &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1

fail() { echo "FAIL: $1"; exit 1; }

[ "$(curl -fsS $BASE/health)" = "ok" ] || fail "health"
echo "PASS health"

echo "ALL PASS"
