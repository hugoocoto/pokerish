#!/usr/bin/env bash

# Exit immediately if a command fails during setup
set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_DIR"

# Ensure no leftover background processes from previous runs are running
pkill -9 -f "server/build/server" 2>/dev/null || true
pkill -9 -f "example_bot" 2>/dev/null || true
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
    echo "==> Tournament finished."
}

# Kill background processes ONLY when interrupted (Ctrl+C / SIGINT / SIGTERM)
trap cleanup INT TERM

echo "==> Starting tournament server (6 seats, 120s levels)..."
./server/build/server --tournament --max-players 6 --level-seconds 120 > /dev/null 2>&1 &
SERVER_PID=$!
PIDS+=("$SERVER_PID")

sleep 1

echo "==> Spawning 3 C++ bots and 2 Python bots..."
./bot/example/build/example_bot --name "CppBot_1" > /dev/null 2>&1 &
PIDS+=($!)
./bot/example/build/example_bot --name "CppBot_2" > /dev/null 2>&1 &
PIDS+=($!)
./bot/example/build/example_bot --name "CppBot_3" > /dev/null 2>&1 &
PIDS+=($!)

bot/python/venv/bin/python bot/python/bot.py --name "PyBot_1" > /dev/null 2>&1 &
PIDS+=($!)
bot/python/venv/bin/python bot/python/bot.py --name "PyBot_2" > /dev/null 2>&1 &
PIDS+=($!)

sleep 1

echo "==> Launching Human GUI client (Seat 6)..."
set +e
./client/build/client --name "HumanPlayer"

echo ""
echo "==> Human GUI client closed."
echo "==> Tournament server & bots are continuing in the background."
echo "==> Waiting for tournament to finish... (Press Ctrl+C to stop all)"
wait "$SERVER_PID" 2>/dev/null || true
