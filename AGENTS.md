# AGENTS.md

Poker Hold'em round simulation: a pure game engine + raylib GUI, with a WebSocket protocol spec (not yet implemented). Two git submodules: `PokerHandEvaluator` (phevaluator, hand strength) and `raylib`. Linux/Wayland only.

## Commands

- `make` — builds `a.out` (raylib GUI, 6 auto-playing bots). First build compiles the submodules via cmake (raylib takes minutes).
- `make test` — builds and runs `build/test_poker`: all 10 tests in one binary, **no per-test filtering** (they run in order in `main()` of tests/test_poker.cpp).
- Tests include `../src/poker.h` directly; plain `CHECK`/`CHECK_EQ` macros, no framework.
- **`make test` is currently slow (minutes)** by design: `Player::ask_for_action` (src/poker.cpp) contains a debug `sleep(1)` plus an 80% "indecision" roll. The user intends to remove this later — keep it, and do not add test workarounds for the slowness.
- GUI prints benign GLFW "Wayland: window position" warnings on stderr — not errors. GLFW is built Wayland-only (no X11), so the GUI needs a Wayland session.

## Architecture

- `src/poker.h` / `src/poker.cpp` — pure engine, no raylib. Everything runs through `Table::step_game(&state, now)`, driven with a monotonic `now`; `Game_State` is the engine/UI boundary.
- `src/main.cpp` — raylib GUI only; keeps a `std::map<int, Card>` keyed by `int(phevaluator::Card)` for card art.
- `src/card.h` — raylib UI card renderer. Confusingly similar to `phevaluator::Card` (the type the engine uses everywhere); do not mix them.
- `API.md` — all API calls (client→server actions, server broadcasts, queries, error codes) are described in `API.md`: a WebSocket/JSON protocol spec for remote clients. Spec only: no networking code exists. `Player::ask_for_action(const Game_State*)` is the intended future hook for remote answers; keep its signature.
- Rules constants in `src/poker.h`: SmallBlind 5, BigBlind 10, StartStack 1000, ActionTimeOut 20 s, MaxPlayers 6.

## Gotchas

- `phevaluator::Card` has no default constructor: `std::map::operator[]` does not compile — use `emplace`/`at`.
- `Player::rank` is private: read hand strength via `hand_value()` (1 = best … 7462 = worst), `describe_category()`, `describe_rank()`; `set_rank()` is only called by `Table::recalc_player_hand_strength()`.
- Bots have `auto_play = true` and answer through `ask_for_action`; non-auto players just fold on timeout. Players busted to 0 chips are rebought to `StartStack` in `end_hand`.
- `Player::last_action` persists for the whole betting round (like `_fold`); it is reset in `start_betting_round` and `end_hand`, not on re-raises.
- Seeded determinism for tests/experiments: `poker_set_seed(unsigned)`.
