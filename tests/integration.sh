#!/bin/sh
set -eu

PORT="${MINIREDIS_TEST_PORT:-6389}"
BIN="${BIN:-./miniredis}"
CLIENT="${CLIENT:-./tests/test_client}"

"$BIN" --port "$PORT" --bind 127.0.0.1 &
SRV_PID=$!
trap 'kill "$SRV_PID" 2>/dev/null || true' EXIT

# Give the server a moment to bind (the client also retries its connect).
sleep 0.2

"$CLIENT" "$PORT" 127.0.0.1
