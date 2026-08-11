#!/usr/bin/env bash

# Fast local smoke run: headless server in cash mode with simulation pacing
# (--simulate), N-1 remote bots (1 Titan + 1 Python + rest C++ example) in
# the background, and the human GUI client in the foreground.
#
# Usage: ./run_fast.sh [N]   (N = seats, 2-10, default 6)

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_DIR"

PLAYERS="${1:-6}"
if ! [[ "$PLAYERS" =~ ^[0-9]+$ ]] || [ "$PLAYERS" -lt 2 ] || [ "$PLAYERS" -gt 10 ]; then
        echo "usage: $0 [N]   (N = seats, 2-10, default 6)" >&2
        exit 1
fi

# Ensure no leftover background processes from previous runs are running
pkill -9 -f "server/build/server" 2>/dev/null || true
pkill -9 -f "example_bot" 2>/dev/null || true
pkill -9 -f "bot/titan/build/titan" 2>/dev/null || true
pkill -9 -f "bot/python/bot.py" 2>/dev/null || true
pkill -9 -f "client/build/client" 2>/dev/null || true
sleep 0.5

echo "==> Building poker binaries..."
make

# Array to keep track of background processes for cleanup
PIDS=()

cleanup() {
    echo ""
    echo "==> Cleaning up background processes..."
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    wait 2>/dev/null || true
    echo "==> Done."
}

# Kill background processes and exit on Ctrl+C / SIGINT / SIGTERM. Bash
# defers the trap until the foreground command (e.g. make) finishes, so
# cleanup + exit here covers signals arriving at any point.
trap 'cleanup; exit 1' INT TERM

echo "==> Starting headless server ($PLAYERS seats, simulate mode)..."
./server/build/server --headless --max-players "$PLAYERS" --simulate > /dev/null 2>&1 &
SERVER_PID=$!
PIDS+=("$SERVER_PID")

sleep 1

echo "==> Spawning $((PLAYERS - 1)) bot(s): 1 Titan + 1 Python + C++ examples..."
./bot/titan/build/titan --name "Titan" > /dev/null 2>&1 &
PIDS+=($!)

if [ "$PLAYERS" -gt 2 ]; then
        bot/python/venv/bin/python bot/python/bot.py --name "PyBot" > /dev/null 2>&1 &
        PIDS+=($!)
fi

# PIDS holds the server plus every bot: keep spawning C++ bots until we
# have one bot per non-human seat (N-1 remote bots total)
i=1
while [ "${#PIDS[@]}" -lt "$PLAYERS" ]; do
        ./bot/example/build/example_bot --name "CppBot_$i" > /dev/null 2>&1 &
        PIDS+=($!)
        i=$((i + 1))
done

sleep 1

echo "==> Launching human GUI client..."
set +e
./client/build/client --name "HumanPlayer" &
CLIENT_PID=$!
PIDS+=("$CLIENT_PID")
# wait() runs the INT/TERM trap, so Ctrl+C (or timeout/kill) kills the
# client too; closing the window ends the script normally
wait "$CLIENT_PID" 2>/dev/null
set -e

cleanup
exit 0
