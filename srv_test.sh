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

code=$(curl -s -o /dev/null -w '%{http_code}' $BASE/boards)
[ "$code" = "302" ] || fail "unauth /boards should 302, got $code"
echo "PASS unauth redirect"

curl -fsS $BASE/login | grep -q '<form method="post" action="/login">' || fail "login page form"
echo "PASS login page"

code=$(curl -s -o /dev/null -w '%{http_code}' -d 'user=ghost&pass=nothing123' $BASE/login)
[ "$code" = "403" ] || fail "bad login should 403, got $code"
echo "PASS bad login"

code=$(curl -s -o /dev/null -w '%{http_code}' $BASE/nowhere)
[ "$code" = "404" ] || fail "unknown path should 404, got $code"
echo "PASS 404"

echo "ALL PASS"
