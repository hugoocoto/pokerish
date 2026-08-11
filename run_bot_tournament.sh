#!/usr/bin/env bash
# run_bot_tournament.sh — start a 4-seat tournament with the GUI server
# and one of each bot (Prometheus, Titan, C++ ExampleBot, Python bot).
#
# Usage: ./run_bot_tournament.sh [OPTIONS]
#
#   --port N           WebSocket port (default: 9000)
#   --level-seconds N  Seconds per blind level (default: 120)
#   --token SECRET     Auth token (passed to server and all bots)
#   --no-simulate      Disable the human-pacing delay (faster games)
#
# The server window opens with the raylib GUI. Close it (or press Ctrl+C here)
# to stop everything cleanly.

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_DIR"

# ── Parse arguments ────────────────────────────────────────────────────────
PORT=9000
LEVEL_SECS=120
TOKEN=""
SIMULATE="--simulate"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)           PORT="$2"; shift 2 ;;
        --level-seconds)  LEVEL_SECS="$2"; shift 2 ;;
        --token)          TOKEN="$2"; shift 2 ;;
        --no-simulate)    SIMULATE=""; shift ;;
        -h|--help)
            echo "Usage: $0 [--port N] [--level-seconds N] [--token SECRET] [--no-simulate]"
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--port N] [--level-seconds N] [--token SECRET] [--no-simulate]" >&2
            exit 1 ;;
    esac
done

# ── Check Python venv exists ───────────────────────────────────────────────
PYTHON="bot/python/venv/bin/python"
if [ ! -x "$PYTHON" ]; then
    echo "==> Python venv not found at $PYTHON. Setting it up..."
    python3 -m venv bot/python/venv
    bot/python/venv/bin/pip install -q websockets
fi

# ── Kill any leftover processes from a previous run ────────────────────────
pkill -9 -f "server/build/server"                2>/dev/null || true
pkill -9 -f "bot/prometheus/build/prometheus"    2>/dev/null || true
pkill -9 -f "bot/titan/build/titan"              2>/dev/null || true
pkill -9 -f "bot/example/build/example_bot"      2>/dev/null || true
pkill -9 -f "bot/python/bot.py"                  2>/dev/null || true
sleep 0.4

# ── Build C++ binaries ─────────────────────────────────────────────────────
echo "==> Building..."
make -j"$(getconf _NPROCESSORS_ONLN)" server bot

# ── Shared token flag helpers ──────────────────────────────────────────────
TOKEN_FLAG=()
[ -n "$TOKEN" ] && TOKEN_FLAG=(--token "$TOKEN")

# ── Background process tracking ───────────────────────────────────────────
PIDS=()

cleanup() {
    echo ""
    echo "==> Stopping bots..."
    for pid in "${PIDS[@]}"; do
        kill -9 "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo "==> Done."
}
trap cleanup INT TERM EXIT

# ── Start the GUI server ───────────────────────────────────────────────────
echo ""
echo "==> Starting tournament server (4 seats, GUI, port $PORT, ${LEVEL_SECS}s levels)..."

SERVER_ARGS=(
    --tournament
    --max-players 4
    --port "$PORT"
    --level-seconds "$LEVEL_SECS"
    "${TOKEN_FLAG[@]}"
)
[ -n "$SIMULATE" ] && SERVER_ARGS+=($SIMULATE)

./server/build/server "${SERVER_ARGS[@]}" &
SERVER_PID=$!
PIDS+=("$SERVER_PID")

# Give the server a moment to open the port
sleep 1.2

# ── Launch all four bots ───────────────────────────────────────────────────
BOT_COMMON=(--port "$PORT" "${TOKEN_FLAG[@]}")

echo "==> Connecting Prometheus..."
./bot/prometheus/build/prometheus "${BOT_COMMON[@]}" --name "Prometheus" \
    > /tmp/prometheus.log 2>&1 &
PIDS+=($!)

echo "==> Connecting Titan..."
./bot/titan/build/titan "${BOT_COMMON[@]}" --name "Titan" \
    > /tmp/titan.log 2>&1 &
PIDS+=($!)

echo "==> Connecting ExampleBot (C++)..."
./bot/example/build/example_bot "${BOT_COMMON[@]}" --name "ExampleBot" \
    > /tmp/example_bot.log 2>&1 &
PIDS+=($!)

echo "==> Connecting PyBot (Python)..."
"$PYTHON" bot/python/bot.py \
    --port "$PORT" --name "PyBot" \
    "${TOKEN_FLAG[@]}" \
    > /tmp/pybot.log 2>&1 &
PIDS+=($!)

echo ""
echo "┌─────────────────────────────────────────┐"
echo "│  Seat 1  →  Prometheus  (elite bot)     │"
echo "│  Seat 2  →  Titan       (strong bot)    │"
echo "│  Seat 3  →  ExampleBot  (C++ random)    │"
echo "│  Seat 4  →  PyBot       (Python random) │"
echo "│                                         │"
echo "│  Server GUI is running in a window.     │"
echo "│  Close the window or press Ctrl+C to    │"
echo "│  stop all bots and exit.                │"
echo "│                                         │"
echo "│  Bot logs:                              │"
echo "│    /tmp/prometheus.log                  │"
echo "│    /tmp/titan.log                       │"
echo "│    /tmp/example_bot.log                 │"
echo "└─────────────────────────────────────────┘"
echo ""

# ── Wait for the server GUI window to close ────────────────────────────────
set +e
wait "$SERVER_PID"
echo ""
echo "==> Server exited. Shutting down bots..."
