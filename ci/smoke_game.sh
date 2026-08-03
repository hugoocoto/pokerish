#!/usr/bin/env bash
# ci/smoke_game.sh - play a live game on the current platform.
#
# Assumes `make` already built server/build/server and
# bot/example/build/example_bot. Starts a headless cash-game server (auto
# bots fill the empty seats) plus one example bot, then checks that the bot
# actually got seated ("joined as seat") and that a hand progressed (a
# broadcast action line) over the real WebSocket protocol. Works on Linux,
# macOS and Windows (MSYS2/MinGW bash).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# Find a free TCP port by probing with bash's /dev/tcp.
PORT=""
for p in $(seq 19900 19950); do
        if ! (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
                PORT=$p
                break
        fi
        exec 3>&- 2>/dev/null || true
done
if [ -z "$PORT" ]; then
        echo "smoke game: no free port in 19900-19950"
        exit 1
fi

BOT_LOG="$(mktemp)"
SRV_LOG="$(mktemp)"
BOT_PID=""
SRV_PID=""
cleanup()
{
        [ -n "$BOT_PID" ] && kill "$BOT_PID" 2>/dev/null || true
        [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null || true
        wait 2>/dev/null || true
        rm -f "$BOT_LOG" "$SRV_LOG"
}
trap cleanup EXIT

./server/build/server --headless --port "$PORT" --action-timeout 2 >"$SRV_LOG" 2>&1 &
SRV_PID=$!

# Wait for the server to start listening.
for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
                exec 3>&-
                break
        fi
        if ! kill -0 "$SRV_PID" 2>/dev/null; then
                echo "smoke game: server died during startup:"
                cat "$SRV_LOG"
                exit 1
        fi
        sleep 0.2
done

./bot/example/build/example_bot --port "$PORT" --name SmokeBot >"$BOT_LOG" 2>&1 &
BOT_PID=$!

# Wait for a seat takeover (queued -> welcome at round end) plus at least one
# broadcast action line, i.e. evidence of a real hand being played.
for _ in $(seq 1 300); do
        if grep -q "joined as seat" "$BOT_LOG" && grep -qE ": P[0-9]+ (fold|call|check|raise|bet)" "$BOT_LOG"; then
                echo "smoke game OK: SmokeBot seated and hand in progress (port $PORT)"
                exit 0
        fi
        if ! kill -0 "$BOT_PID" 2>/dev/null; then break; fi
        if ! kill -0 "$SRV_PID" 2>/dev/null; then break; fi
        sleep 0.2
done

echo "smoke game FAILED (60s deadline):"
cat "$BOT_LOG"
echo "--- server log ---"
cat "$SRV_LOG"
exit 1
