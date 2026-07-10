#!/bin/sh
# srv_test.sh — integration gauntlet for solsrv: builds, seeds a scratch db,
# boots the server, and drives the real HTTP surface with curl. Full-strength
# PBKDF2 makes each login ~0.5s; the whole run is ~10s by design.
set -eu

PORT=18080
BASE="http://127.0.0.1:$PORT"
DB=srv_itest.db
JAR=srv_itest.cookies
PW=hunter2boogaloo

rm -f "$DB" "$DB-wal" "$DB-shm" "$JAR"
./build.sh server >/dev/null

printf '%s' "$PW" | ./solsrv -d "$DB" --adduser fran
./solsrv -d "$DB" --seed

./solsrv -p 127.0.0.1:$PORT -d "$DB" &
SRV=$!
trap 'kill $SRV 2>/dev/null || true; rm -f "$JAR"' EXIT
sleep 1

fail() { echo "FAIL: $1"; exit 1; }

[ "$(curl -fsS $BASE/health)" = "ok" ] || fail "health"
echo "PASS health"

code=$(curl -s -o /dev/null -w '%{http_code}' $BASE/boards)
[ "$code" = "302" ] || fail "unauth /boards should 302, got $code"
echo "PASS unauth redirect"

curl -fsS $BASE/login | grep -q '<form method="post" action="/login">' || fail "login page form"
echo "PASS login page"

code=$(curl -s -o /dev/null -w '%{http_code}' $BASE/nowhere)
[ "$code" = "404" ] || fail "unknown path should 404, got $code"
echo "PASS 404"

code=$(curl -s -o /dev/null -w '%{http_code}' -d "user=fran&pass=wrongwrong" $BASE/login)
[ "$code" = "403" ] || fail "bad login should 403, got $code"
echo "PASS bad login"

code=$(curl -s -o /dev/null -w '%{http_code}' -c "$JAR" -d "user=fran&pass=$PW" $BASE/login)
[ "$code" = "302" ] || fail "good login should 302, got $code"
curl -fsS -b "$JAR" $BASE/boards | grep -q 'Welcome to Solarium' || fail "boards shows seeded board"
curl -fsS -b "$JAR" $BASE/boards | grep -q 'Reading list' || fail "boards shows second board"
echo "PASS login + seeded boards"

./solsrv -d "$DB" --seed 2>/dev/null && fail "re-seed should refuse" || true
echo "PASS seed refuses twice"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST $BASE/logout)
[ "$code" = "302" ] || fail "logout should 302, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" $BASE/boards)
[ "$code" = "302" ] || fail "session dead after logout, got $code"
echo "PASS logout"

i=0
while [ $i -lt 10 ]; do
    curl -s -o /dev/null -d "user=fran&pass=wrongwrong" $BASE/login
    i=$((i+1))
done
code=$(curl -s -o /dev/null -w '%{http_code}' -d "user=fran&pass=$PW" $BASE/login)
[ "$code" = "429" ] || fail "throttle should 429 after 10 fails, got $code"
echo "PASS throttle"

code=$(curl -s -o /dev/null -w '%{http_code}' -H 'X-Forwarded-For: 10.9.9.9' -d "user=fran&pass=wrongwrong" $BASE/login)
[ "$code" = "403" ] || fail "XFF re-keys throttle: expected 403 fresh bucket, got $code"
echo "PASS throttle XFF keying"

echo "ALL PASS"
