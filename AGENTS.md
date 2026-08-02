# AGENTS.md

Poker Hold'em round simulation: a pure game engine + WebSocket server with a raylib GUI (all-in-one binary), with a WebSocket/JSON protocol (API.md) for remote clients. Git submodules under `thirdparty/`: `PokerHandEvaluator` (phevaluator, hand strength), `raylib` and `nlohmann` (JSON). Linux/Wayland only.

## Commands

- `make` (root) — builds `server/build/server` (raylib GUI + WebSocket server, 5 auto-playing local bots + 1 remote client seat). First build compiles the submodules via cmake (raylib takes minutes). Equivalent to `make -C server`.
- Run the GUI server: `./server/build/server [--port 9000] [--host 127.0.0.1] [--token SECRET]` (loopback by default; `--host 0.0.0.0` for LAN); connect a random client: `./bot/example/build/example_bot --port 9000 [--token SECRET]`. `--headless` runs the same server without the raylib GUI (useful for debugging on a headless box). When the server runs with `--token`, clients must send the same token in `hello`.
- `make bot` (root) — builds `bot/example/build/example_bot` (a single-file remote client with a random person name like `Bob`, for the GUI server's remote seats). Equivalent to `make -C bot/example`.
- `make test` — builds and runs `test/build/test_poker`, `test/build/test_proto`, `test/build/test_server`: all tests in one binary each, **no per-test filtering** (they run in order in `main()`). Pushed to GitHub, the `.github/workflows/tests.yml` workflow runs the same `make test` (submodules included, needs `libwebsockets-dev`/`libcap-dev`/`libsystemd-dev`).
- Tests include `../server/src/poker.h` and `../bot/example/bot.cpp` directly; plain `CHECK`/`CHECK_EQ` macros, no framework.
- **Engine bots act instantly**: `Player::ask_for_action` decides immediately (no thinking delay). The ~1 s "thinking" delay + 40% indecision (per try) lives in the remote `Bot` client (bot/example/bot.cpp `pump`).
- GUI prints benign GLFW "Wayland: window position" warnings on stderr — not errors. GLFW is built Wayland-only (no X11), so the GUI needs a Wayland session.

## Architecture

The repo is split into two independent projects plus tests:

- `server/` — the whole game: engine, protocol, WebSocket server and raylib GUI.
  - `server/src/poker.h` / `server/src/poker.cpp` — pure engine, no raylib. Everything runs through `Table::step_game(&state, now)`, driven with a monotonic `now`; `Game_State` is the engine/UI boundary.
  - `server/src/server_main.cpp` — raylib GUI + WebSocket server entry point: creates `Server srv(port, host, token)`, drives it with `srv.tick(GetTime())` every frame (lws_service + engine step + event detection), and draws the table; card art built from `decks/jorels`, all cards visible (the binary cd's up to the repo root to find `decks/`).
  - `server/src/card.h` — raylib UI card renderer. Confusingly similar to `phevaluator::Card` (the type the engine uses everywhere); do not mix them.
  - `server/src/server.cpp` / `server/src/server.h` — WebSocket server. `run()` is the headless loop (used by tests); the GUI entry uses `tick(now)` instead. Broadcasts a full state snapshot to all clients whenever the turn changes, plus targeted `your_turn` messages. Seat changes only happen at the end of a round: `hello` reserves a bot seat (`queued`), the client takes over with a full entry stack when the round ends (`welcome`); clients leaving mid-hand are folded and a bot resumes the seat at the next round end.
  - `server/src/proto.{h,cpp}` — the API.md protocol layer (message parsing, validation, state JSON). Uses nlohmann + the engine.
- `bot/` — independent clients: WebSocket + JSON only (API.md), **no engine/protocol headers**. `bot/example/bot.cpp` is the single-file C++ client (standalone binary, or a test client when `test/test_server.cpp` includes it without `-DBOT_EXAMPLE_STANDALONE`); `bot/python/bot.py` is the same bot in Python (`bot/python/venv` holds its deps, run `venv/bin/python bot.py`). Both are intentionally small examples of the API.md protocol, meant to be extended into smarter bots.
- `API.md` — all API calls (client→server actions, server broadcasts, queries, error codes) are described in `API.md`: a WebSocket/JSON protocol spec for remote clients. `Player::ask_for_action(const Game_State*)` is the hook bots use to answer; keep its signature.
- Rules constants in `server/src/poker.h`: SmallBlind 5, BigBlind 10, StartStack 1000, ActionTimeOut 20 s, MaxPlayers 6.
- `deps.mk` — shared make rules for the thirdparty submodule libraries (raylib, phevaluator), included by the root and server Makefiles.

## Gotchas

- `phevaluator::Card` has no default constructor: `std::map::operator[]` does not compile — use `emplace`/`at`.
- `Player::rank` is private: read hand strength via `hand_value()` (1 = best … 7462 = worst), `describe_category()`, `describe_rank()`; `set_rank()` is only called by `Table::recalc_player_hand_strength()`.
- Bots have `auto_play = true` and answer through `ask_for_action`; non-auto players just fold on timeout. Players busted to 0 chips are rebought to `StartStack` in `end_hand`.
- `Player::last_action` persists for the whole betting round (like `_fold`); it is reset in `start_betting_round` and `end_hand`, not on re-raises.
- Seeded determinism for tests/experiments: `poker_set_seed(unsigned)`.
- **`lws_service()` ignores its timeout argument** (blocking since lws 3.2) on both server and client contexts. Any loop that needs to keep stepping (GUI `Server::tick()`, test loops pumping a client context) must have a ticker thread calling `lws_cancel_service()` ~every 16 ms — the server has one built in, client contexts in tests need their own.
