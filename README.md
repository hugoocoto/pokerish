# Poker Hold'em

A Texas Hold'em poker simulation: a pure game engine bundled with a WebSocket
server and an optional raylib table GUI in a single binary. Remote clients —
human players or bots — connect over WebSockets and talk JSON, following the
protocol in [API.md](API.md).

Two game modes:

- **Cash game** (default): 2–10 seats, empty seats are filled by auto bots,
  busted players are rebought — the game never ends.
- **Tournament** (`--tournament`): sit-and-go with rising blind levels, antes,
  a countdown, and winner-takes-all elimination.

Written in C++ (C++17) with no dependencies beyond cmake, libwebsockets and
the bundled third-party submodules. Linux, macOS and Windows.

## Documentation

| Doc | What it covers |
|-----|----------------|
| [HOW_TO_HOST.md](HOW_TO_HOST.md) | Running the server: every flag, both game modes, tournament details, examples. Start here to host a game. |
| [HOW_TO_BOT.md](HOW_TO_BOT.md) | Writing a custom bot that connects over WebSockets: the message lifecycle, actions, queries. |
| [API.md](API.md) | The full WebSocket/JSON protocol reference: every message, query, state field and error code. |
| [AGENTS.md](AGENTS.md) | Architecture notes, build internals and gotchas — for developers working on the code. |

## Features

- All-in-one server binary: game engine + WebSocket server + raylib table GUI
  (run with `--headless` to skip the GUI).
- Remote clients over WebSocket/JSON (see [API.md](API.md)): a raylib play
  client for humans (`client/`) and three reference bots (`bot/`).
- Instant auto bots fill empty cash-game seats; human-like "thinking" delays
  with `--simulate`.
- Tournament mode with level schedule, antes, countdown and configurable
  end-of-game behavior (`stay`, `exit`, `restart`).
- Hand history export in PokerStars format (`--export-dir hands`).
- Card artwork from `decks/`; seeded determinism for tests and experiments
  (`poker_set_seed`).

## Repository layout

| Path | Contents |
|------|----------|
| `server/` | The whole game: engine (`src/poker.{h,cpp}`), protocol (`src/proto.{h,cpp}`), WebSocket server (`src/server.{h,cpp}`), tournament logic, hand-history export, raylib GUI entry point (`src/server_main.cpp`). |
| `bot/` | Independent clients: `example/` (minimal C++ bot), `titan/` (stronger strategy bot), `python/` (Python version of the example bot). |
| `client/` | raylib GUI play client for a human (Fold/Call/Raise buttons, own cards face up). |
| `test/` | Unit tests, one binary per suite (see [Testing](#testing)). |
| `ci/` | `smoke_game.sh`: live headless game (real server + bot over WebSocket). |
| `decks/` | Card artwork (PNG). |
| `hands/` | PokerStars-format hand history exports. |
| `thirdparty/` | Git submodules: `PokerHandEvaluator` (hand strength), `raylib` (GUI), `nlohmann` (JSON). |
| `Makefile`, `deps.mk` | Umbrella build + shared rules for the submodule libraries. |
| `run_fast.sh`, `run_tournament.sh` | One-shot scripts that build, start a server, spawn bots and launch the GUI client. |

## Getting the source

The third-party libraries are git submodules, so clone recursively:

```sh
git clone --recursive <repo-url>
cd poker
```

If you already cloned without `--recursive`:

```sh
git submodule update --init --recursive
```

## Building

### Platform dependencies

All platforms need a C++ compiler (gcc/clang; MinGW-w64 gcc on Windows),
`make`, `cmake` and the `libwebsockets` dev headers, plus the platform
packages for the raylib GUI backend:

| Platform | Install |
|----------|---------|
| Linux (Wayland) | `sudo apt install libwebsockets-dev libcap-dev libsystemd-dev libwayland-dev libwayland-bin libxkbcommon-dev libgl1-mesa-dev` |
| Linux (X11) | `sudo apt install libwebsockets-dev libcap-dev libsystemd-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxxf86vm-dev libxkbcommon-dev libgl1-mesa-dev` |
| Arch Linux (Wayland) | `sudo pacman -S libwebsockets libcap systemd-libs wayland wayland-utils libxkbcommon mesa` |
| Arch Linux (X11) | `sudo pacman -S libwebsockets libcap systemd-libs libx11 libxrandr libxinerama libxcursor libxi libxxf86vm libxkbcommon mesa` |
| macOS | `brew install libwebsockets cmake` |
| Windows (MSYS2/MinGW-w64) | `pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-libwebsockets make` |

Then build:

```sh
make            # builds all binaries (parallel-safe: make -j)
```

This produces:

- `server/build/server` — the game server (+ GUI)
- `bot/example/build/example_bot` — minimal C++ bot
- `bot/titan/build/titan` — stronger strategy bot
- `client/build/client` — raylib human play client

The first build compiles the third-party submodules via cmake; raylib takes a
few minutes. The raylib backend is auto-detected (`wayland` on a Wayland
session, `x11` otherwise, `macos` on macOS, `windows` on MSYS2/MinGW) — force
one with `make RAYLIB_PLATFORM=x11` (or `wayland`, `macos`, `windows`); each
backend builds into its own directory, so switching needs no cache clearing.

Useful targets:

| Target | What it builds |
|--------|----------------|
| `make server` | `server/build/server` only |
| `make bot` | `bot/example` + `bot/titan` |
| `make client` | `client/build/client` only |
| `make test` | builds and runs the test binaries (see [Testing](#testing)) |
| `make clean` | removes the build directories |
| `make distclean` | `clean` + wipes the submodule build caches |

There is no `make install`: the binaries run in place from their build
directories (the server finds the card art in `decks/` relative to the repo
root).

## Running

### Cash game

```sh
# Private game on this machine, raylib table GUI:
./server/build/server

# LAN game with human-like pacing (bots "think" 1–3 s):
./server/build/server --host 0.0.0.0 --simulate

# Headless server (no GUI) on a remote box:
./server/build/server --headless --host 0.0.0.0 --token SECRET
```

Connect players:

```sh
./client/build/client                 # human play client (GUI)
./bot/example/build/example_bot       # example bot
./bot/titan/build/titan               # stronger bot
bot/python/venv/bin/python bot/python/bot.py   # Python bot (deps in bot/python/venv)
```

### Tournament

```sh
./server/build/server --tournament --max-players 4 --level-seconds 120 \
                      --tournament-end restart
```

### One-shot scripts

- `./run_fast.sh [N]` — builds everything, starts a headless cash server (N
  seats, `--simulate`), spawns N-1 bots and launches the human GUI client.
- `./run_tournament.sh` — same, but a 6-player tournament with 1 Titan +
  2 C++ + 2 Python bots and your play client in the foreground.

See [HOW_TO_HOST.md](HOW_TO_HOST.md) for the complete flag reference, both
game modes, and more examples.

## Testing

```sh
make test
```

Builds and runs five test binaries in `test/build/` (a few seconds in total):

| Binary | Covers |
|--------|--------|
| `test_poker` | the pure game engine (betting, hand evaluation, pots) |
| `test_proto` | protocol parsing/validation, state JSON |
| `test_server` | the WebSocket server over real sockets |
| `test_tournament` | tournament flow with tiny level/countdown durations |
| `test_pokerstars_export` | PokerStars-format hand history |

`ci/smoke_game.sh` additionally plays a live headless game (real server +
example bot over WebSocket). The same build + tests + smoke game run on GitHub
Actions for Linux (Wayland and X11), macOS and Windows.
