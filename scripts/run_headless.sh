#!/usr/bin/env bash

# run_headless.sh — headless tournament server with all bots in the
# background and your human GUI client on the last seat in the foreground.
# Close the client window (or press Ctrl+C) to stop everything.
#
# Usage: ./run_headless.sh [N] [--port PORT] [--level-seconds SECS]
#                          [--token SECRET] [--no-simulate]
#   N               total seats (2-10, default 6); N-1 bots are spawned
#   --port PORT     WebSocket port (default 9000)
#   --level-seconds Blind level duration in seconds (default 120)
#   --token SECRET  Auth token (passed to server and all bots)
#   --no-simulate   Disable the human-pacing delay (faster games)

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

# ── Parse arguments ────────────────────────────────────────────────────────
PLAYERS=6
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
        [0-9]*)           PLAYERS="$1"; shift ;;
        -h|--help)
            echo "Usage: $0 [N] [--port PORT] [--level-seconds SECS] [--token SECRET] [--no-simulate]"
            exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

if ! [[ "$PLAYERS" =~ ^[0-9]+$ ]] || [ "$PLAYERS" -lt 2 ] || [ "$PLAYERS" -gt 10 ]; then
    echo "usage: $0 [N (2-10, default 6)] [--port PORT] [--level-seconds SECS] [--token SECRET] [--no-simulate]" >&2
    exit 1
fi

# ── Python venvs ───────────────────────────────────────────────────────────
if [ ! -x bot/python/venv/bin/python ]; then
    echo "==> Setting up bot/python venv..."
    python3 -m venv bot/python/venv
    bot/python/venv/bin/pip install -q websockets
fi
if [ ! -x bot/nemesis/venv/bin/python ]; then
    echo "==> Setting up bot/nemesis venv..."
    python3 -m venv bot/nemesis/venv
    bot/nemesis/venv/bin/pip install -q -r bot/nemesis/requirements.txt
fi

# ── Kill any leftover processes from a previous run ────────────────────────
pkill -9 -f "server/build/server"                  2>/dev/null || true
pkill -9 -f "bot/prometheus/build/prometheus"      2>/dev/null || true
pkill -9 -f "bot/titan/build/titan"                2>/dev/null || true
pkill -9 -f "bot/example/build/example_bot"        2>/dev/null || true
pkill -9 -f "bot/python/bot.py"                    2>/dev/null || true
pkill -9 -f "bot/nemesis/bot.py"                   2>/dev/null || true
pkill -9 -f "client/build/client"                  2>/dev/null || true
sleep 0.5

# ── Build ──────────────────────────────────────────────────────────────────
echo "==> Building server, bots and client..."
make -j"$(getconf _NPROCESSORS_ONLN)" server bot client

TOKEN_FLAG=()
[ -n "$TOKEN" ] && TOKEN_FLAG=(--token "$TOKEN")

PIDS=()

cleanup() {
    echo ""
    echo "==> Stopping server and bots..."
    for pid in "${PIDS[@]}"; do
        kill -9 "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo "==> Done."
}
trap cleanup EXIT INT TERM

# ── Start the headless tournament server ───────────────────────────────────
echo ""
echo "==> Starting headless tournament server ($PLAYERS seats, port $PORT, ${LEVEL_SECS}s levels)..."

SERVER_ARGS=(
    --tournament
    --headless
    --max-players "$PLAYERS"
    --port "$PORT"
    --level-seconds "$LEVEL_SECS"
    "${TOKEN_FLAG[@]}"
)
[ -n "$SIMULATE" ] && SERVER_ARGS+=($SIMULATE)

./server/build/server "${SERVER_ARGS[@]}" > /dev/null 2>&1 &
SERVER_PID=$!
PIDS+=("$SERVER_PID")
sleep 1.2

# ── Spawn bots: Prometheus, Titan, Nemesis, PyBot, then C++ fillers ────────
BOT_COMMON=(--port "$PORT" "${TOKEN_FLAG[@]}")
TO_SPAWN=$((PLAYERS - 1))

spawn_bot() {
    local name="$1"
    shift
    [ "$TO_SPAWN" -le 0 ] && return
    "$@" --name "$name" > /dev/null 2>&1 &
    PIDS+=($!)
    TO_SPAWN=$((TO_SPAWN - 1))
    sleep 0.3
}

echo "==> Spawning $((PLAYERS - 1)) bot(s)..."
spawn_bot "Prometheus" ./bot/prometheus/build/prometheus "${BOT_COMMON[@]}"
spawn_bot "Titan"      ./bot/titan/build/titan "${BOT_COMMON[@]}"
spawn_bot "Nemesis"    bot/nemesis/venv/bin/python bot/nemesis/bot.py "${BOT_COMMON[@]}"
spawn_bot "PyBot"      bot/python/venv/bin/python bot/python/bot.py "${BOT_COMMON[@]}"
i=1
while [ "$TO_SPAWN" -gt 0 ]; do
    spawn_bot "CppBot_$i" ./bot/example/build/example_bot "${BOT_COMMON[@]}"
    i=$((i + 1))
done

echo ""
echo "==> Launching your GUI client on the last seat..."
set +e
./client/build/client "${BOT_COMMON[@]}" --name "HumanPlayer"
echo ""
echo "==> Client closed."
