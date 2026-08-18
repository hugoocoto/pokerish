#!/usr/bin/env bash
# run_nemesis_tournament.sh — spin up a bot-only tournament with Nemesis vs Prometheus + Titan.
#
# Usage: ./scripts/run_nemesis_tournament.sh [N] [--port PORT] [--level-seconds SECS]
#   N                 total seats (2-9, default 4)
#   --port PORT       WebSocket port (default 9000)
#   --level-seconds   blind level duration in seconds (default 60)
#
# Seat assignment (for N=4, the default):
#   Seat 0: Prometheus  (C++ elite bot)
#   Seat 1: Titan       (C++ strong bot)
#   Seat 2: example_bot (C++ simple bot, fills extra seats)
#   Seat 3: Nemesis     (Python — this repo's new bot)
#   ...extra seats filled with example_bot...

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

# --- parse args ---
PLAYERS=4
PORT=9000
LEVEL_SECS=60

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)         PORT="$2";       shift 2 ;;
        --level-seconds) LEVEL_SECS="$2"; shift 2 ;;
        [0-9]*)         PLAYERS="$1";    shift   ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

if ! [[ "$PLAYERS" =~ ^[0-9]+$ ]] || [ "$PLAYERS" -lt 2 ] || [ "$PLAYERS" -gt 9 ]; then
    echo "usage: $0 [N (2-9)] [--port PORT] [--level-seconds SECS]" >&2
    exit 1
fi

# --- venv setup for Nemesis ---
NEMESIS_DIR="$REPO_DIR/bot/nemesis"
VENV="$NEMESIS_DIR/venv"
if [ ! -f "$VENV/bin/python" ]; then
    echo "==> Setting up Nemesis Python venv..."
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install --quiet -r "$NEMESIS_DIR/requirements.txt"
    echo "==> Nemesis venv ready."
fi

# --- cleanup leftover processes ---
pkill -9 -f "server/build/server"   2>/dev/null || true
pkill -9 -f "bot/prometheus/build"  2>/dev/null || true
pkill -9 -f "bot/titan/build/titan" 2>/dev/null || true
pkill -9 -f "bot/example/build/example_bot" 2>/dev/null || true
pkill -9 -f "bot/nemesis/bot.py"    2>/dev/null || true
sleep 0.5

# --- build C++ binaries ---
echo "==> Building server, Prometheus, Titan, example_bot..."
make -j bot server

PIDS=()

cleanup() {
    echo ""
    echo "==> Cleaning up..."
    for pid in "${PIDS[@]}"; do
        kill -9 "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo "==> Done."
}
trap cleanup EXIT INT TERM

# --- start server ---
echo "==> Starting headless tournament server (port=$PORT, seats=$PLAYERS, level=${LEVEL_SECS}s)..."
./server/build/server \
    --headless \
    --tournament \
    --max-players "$PLAYERS" \
    --port "$PORT" \
    --level-seconds "$LEVEL_SECS" \
    --tournament-end restart \
    > /tmp/nemesis_server.log 2>&1 &
SERVER_PID=$!
PIDS+=("$SERVER_PID")
sleep 1

# --- start bots ---
echo "==> Starting Prometheus..."
./bot/prometheus/build/prometheus --port "$PORT" --name "Prometheus" > /tmp/nemesis_prometheus.log 2>&1 &
PIDS+=($!)
sleep 0.3

if [ "$PLAYERS" -ge 3 ]; then
    echo "==> Starting Titan..."
    ./bot/titan/build/titan --port "$PORT" --name "Titan" > /tmp/nemesis_titan.log 2>&1 &
    PIDS+=($!)
    sleep 0.3
fi

# Fill extra seats with example_bot up to PLAYERS-1 total bots
i=1
while [ "${#PIDS[@]}" -lt "$PLAYERS" ]; do
    echo "==> Starting example_bot #$i..."
    ./bot/example/build/example_bot --port "$PORT" --name "CppBot_$i" > /dev/null 2>&1 &
    PIDS+=($!)
    sleep 0.2
    i=$((i + 1))
done

# --- start Nemesis last (fills final seat) ---
echo "==> Starting Nemesis..."
"$VENV/bin/python" "$NEMESIS_DIR/bot.py" --port "$PORT" --name "Nemesis" -v \
    > /tmp/nemesis_bot.log 2>&1 &
PIDS+=($!)

echo ""
echo "==> Tournament running. Logs:"
echo "    Server:     /tmp/nemesis_server.log"
echo "    Nemesis:    /tmp/nemesis_bot.log"
echo "    Prometheus: /tmp/nemesis_prometheus.log"
if [ "$PLAYERS" -ge 3 ]; then
echo "    Titan:      /tmp/nemesis_titan.log"
fi
echo ""
echo "Press Ctrl+C to stop."

# Tail Nemesis log so the user can see what it's deciding in real time
tail -f /tmp/nemesis_bot.log
