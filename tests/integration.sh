#!/bin/sh
set -eu

PORT="${MINIREDIS_TEST_PORT:-6389}"
BIN="${BIN:-./miniredis}"
CLIENT="${CLIENT:-./tests/test_client}"

# Run the full battery plus the concurrent burst test against a server started
# with the given extra arguments, then shut it down.
run_phase() {
    "$BIN" --port "$PORT" --bind 127.0.0.1 "$@" &
    SRV_PID=$!
    trap 'kill "$SRV_PID" 2>/dev/null || true' EXIT

    # Give the server a moment to bind (the client also retries its connect).
    sleep 0.2

    "$CLIENT" "$PORT" 127.0.0.1 full
    "$CLIENT" "$PORT" 127.0.0.1 burst
    "$CLIENT" "$PORT" 127.0.0.1 biginput

    kill "$SRV_PID" 2>/dev/null || true
    wait "$SRV_PID" 2>/dev/null || true
    trap - EXIT
    sleep 0.2
}

if [ "$(uname -s)" = "Linux" ]; then
    run_phase --io epoll
fi
run_phase --io select

# ---- active expiration: the periodic expire cycle removes short-TTL keys ----
"$BIN" --port "$PORT" --bind 127.0.0.1 &
SRV_PID=$!
trap 'kill "$SRV_PID" 2>/dev/null || true' EXIT
sleep 0.2
"$CLIENT" "$PORT" 127.0.0.1 expire
kill "$SRV_PID" 2>/dev/null || true
wait "$SRV_PID" 2>/dev/null || true
trap - EXIT
