#!/bin/sh
set -eu

PORT="${MINIREDIS_TEST_PORT:-6389}"
BIN="${BIN:-./miniredis}"
CLIENT="${CLIENT:-./tests/test_client}"

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

start_server() {
    # $@ = extra server args
    "$BIN" --port "$PORT" --bind 127.0.0.1 "$@" &
    SRV_PID=$!
    sleep 0.3
}

stop_server() {
    # $1 = signal (TERM for graceful shutdown, KILL for crash)
    kill "-$1" "$SRV_PID" 2>/dev/null || true
    wait "$SRV_PID" 2>/dev/null || true
    sleep 0.2
}

# ---- test 1: AOF crash recovery (kill -9, no graceful save) ----
start_server --aof "$DIR/a.aof" --rdb "$DIR/a.rdb"
"$CLIENT" "$PORT" 127.0.0.1 write
stop_server KILL

start_server --aof "$DIR/a.aof" --rdb "$DIR/a.rdb"
"$CLIENT" "$PORT" 127.0.0.1 verify
stop_server TERM

# ---- test 2: RDB recovery via graceful shutdown (SIGTERM saves the snapshot) ----
start_server --rdb "$DIR/r.rdb"
"$CLIENT" "$PORT" 127.0.0.1 write
stop_server TERM

start_server --rdb "$DIR/r.rdb"
"$CLIENT" "$PORT" 127.0.0.1 verify
stop_server TERM

# ---- test 3: AOF rewrite -> crash -> recover from the compacted AOF ----
# The write issued right after BGREWRITEAOF must survive via the rewrite
# buffer, even though it never hit the pre-rewrite AOF.
start_server --aof "$DIR/rw.aof"
"$CLIENT" "$PORT" 127.0.0.1 write
"$CLIENT" "$PORT" 127.0.0.1 rewrite
stop_server KILL

start_server --aof "$DIR/rw.aof"
"$CLIENT" "$PORT" 127.0.0.1 verifyrw
stop_server TERM

echo "integration_persist: all tests passed"
