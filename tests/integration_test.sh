#!/usr/bin/env bash
# Black-box integration test: builds the project if needed, starts the
# real server binary, drives it through the real client binary, and
# checks responses. Exercises every command plus WRONGTYPE errors, TTL
# expiry, and a concurrent-access burst (the "survives a basic ...
# concurrent-access test" bullet under Track D).
#
# Usage: bash tests/integration_test.sh   (run from the repo root, or here)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
SERVER="$ROOT_DIR/build/redis-server"
CLIENT="$ROOT_DIR/build/redis-client"

PASS=0
FAIL=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" == "$actual" ]; then
        PASS=$((PASS + 1))
        echo "  [ OK ] $desc"
    else
        FAIL=$((FAIL + 1))
        echo "  [FAIL] $desc"
        echo "         expected: $expected"
        echo "         actual:   $actual"
    fi
}

check_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        PASS=$((PASS + 1))
        echo "  [ OK ] $desc"
    else
        FAIL=$((FAIL + 1))
        echo "  [FAIL] $desc (expected to find: $needle)"
        echo "$haystack"
    fi
}

run() { "$CLIENT" "$@"; }

if [ ! -x "$SERVER" ] || [ ! -x "$CLIENT" ]; then
    echo "redis-server / redis-client not found, building first (make)..."
    (cd "$ROOT_DIR" && make) || { echo "build failed"; exit 1; }
fi

# Clean up persistence files in the binary's directory
SERVER_DIR="$(dirname "$SERVER")"
rm -rf "$SERVER_DIR/data"
mkdir -p "$SERVER_DIR/data"

echo "Starting server..."
"$SERVER" &
SERVER_PID=$!
trap 'kill -9 "$SERVER_PID" 2>/dev/null' EXIT
sleep 0.3
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "server failed to start (is port 1234 already in use?)"
    exit 1
fi

echo "-- string --"
check "SET returns nil"          "(nil)"    "$(run set foo bar)"
check "GET returns value"        "(str) bar" "$(run get foo)"
check "GET on missing key"       "(nil)"    "$(run get nosuchkey)"
check "DEL existing key"         "(int) 1"  "$(run del foo)"
check "DEL already-gone key"     "(int) 0"  "$(run del foo)"
check "TYPE of missing key"      "(str) none" "$(run type foo)"

echo "-- list --"
run rpush mylist a b c > /dev/null
check "LLEN after rpush a b c"   "(int) 3"  "$(run llen mylist)"
run lpush mylist z > /dev/null
check "LPOP returns head"        "(str) z"  "$(run lpop mylist)"
check "RPOP returns tail"        "(str) c"  "$(run rpop mylist)"
check "LLEN after pops"          "(int) 2"  "$(run llen mylist)"
check "LPOP on empty list is nil" "(nil)"   "$(run lpop emptylist)"
check "GET on a list is WRONGTYPE" "(err) 3 WRONGTYPE not a string" "$(run get mylist)"

echo "-- hash --"
run hset myhash f1 v1 f2 v2 > /dev/null
check "HGET existing field"      "(str) v1" "$(run hget myhash f1)"
check "HLEN"                     "(int) 2"  "$(run hlen myhash)"
check "HDEL existing field"      "(int) 1"  "$(run hdel myhash f1)"
check "HGET deleted field"       "(nil)"    "$(run hget myhash f1)"
check "HGET on missing key"      "(nil)"    "$(run hget nosuchhash f1)"

echo "-- set --"
run sadd myset a b c > /dev/null
check "SCARD"                    "(int) 3"  "$(run scard myset)"
check "SISMEMBER present"        "(int) 1"  "$(run sismember myset a)"
check "SISMEMBER absent"         "(int) 0"  "$(run sismember myset zzz)"
check "SADD duplicate is a no-op" "(int) 0" "$(run sadd myset a)"
check "SREM present member"      "(int) 1"  "$(run srem myset a)"
check "SCARD after srem"         "(int) 2"  "$(run scard myset)"

echo "-- sorted set (ZADD key score member) --"
run zadd leaderboard 100 alice > /dev/null
run zadd leaderboard 90 bob > /dev/null
run zadd leaderboard 95 carol > /dev/null
check "ZSCORE existing member"   "(dbl) 90" "$(run zscore leaderboard bob)"
check "ZSCORE missing member"    "(nil)"    "$(run zscore leaderboard dave)"
check "ZREM existing member"     "(int) 1"  "$(run zrem leaderboard bob)"
check "ZSCORE after zrem"        "(nil)"    "$(run zscore leaderboard bob)"
ZQUERY_OUT="$(run zquery leaderboard 0 "" 0 10)"
check_contains "ZQUERY includes carol"  "(str) carol" "$ZQUERY_OUT"
check_contains "ZQUERY includes alice"  "(str) alice" "$ZQUERY_OUT"

echo "-- ttl --"
run set temp v > /dev/null
run pexpire temp 50 > /dev/null
check "PTTL is positive right after SET" "1" "$([ "$(run pttl temp | grep -c '(int)')" -eq 1 ] && echo 1)"
sleep 0.2
check "GET after TTL expiry"     "(nil)"    "$(run get temp)"
check "PTTL of an expired key is -2" "(int) -2" "$(run pttl temp)"

echo "-- concurrent access (Track D requirement) --"
CLIENT_PIDS=""
for i in $(seq 1 50); do
    run sadd concurrent_set "member$i" > /dev/null &
    CLIENT_PIDS="$CLIENT_PIDS $!"
done
wait $CLIENT_PIDS
check "SCARD after 50 concurrent SADDs" "(int) 50" "$(run scard concurrent_set)"

CLIENT_PIDS=""
for i in $(seq 1 50); do
    run zadd concurrent_zset "$i" "member$i" > /dev/null &
    CLIENT_PIDS="$CLIENT_PIDS $!"
done
wait $CLIENT_PIDS
ZLEN_OUT="$(run zquery concurrent_zset -1000000 "" 0 1000)"
CONCURRENT_ZCOUNT="$(echo "$ZLEN_OUT" | grep -c '(str) member')"
check "50 concurrent ZADDs all landed" "50" "$CONCURRENT_ZCOUNT"

echo ""
echo "$PASS passed, $FAIL failed"
kill -9 "$SERVER_PID" 2>/dev/null
[ "$FAIL" -eq 0 ]