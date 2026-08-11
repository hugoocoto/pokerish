# Prometheus — Elite Poker Bot

Prometheus is an advanced poker bot for the [poker server](../../API.md) WebSocket/JSON protocol. It is designed to beat both other bots and human players through a combination of GTO-calibrated strategy, opponent modeling, board texture analysis, and dynamic bet sizing.

---

## Build

Prometheus uses the same build system as the rest of the project. From the **repo root**:

```bash
make bot           # builds example_bot, titan, and prometheus
```

Or build only Prometheus:

```bash
make -C bot/prometheus
```

The binary is written to `bot/prometheus/build/prometheus`.

> **Dependencies** (all vendored under `thirdparty/`):  
> `libwebsockets` (WebSocket transport) · `nlohmann/json` (JSON) · `phevaluator` (hand evaluation)  
> No additional packages are needed beyond a standard C++17 compiler.

---

## Run

```bash
./bot/prometheus/build/prometheus [OPTIONS]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--port N` | `9000` | Server port |
| `--host IP` | `127.0.0.1` | Server IP address |
| `--token SECRET` | *(none)* | Auth token (required if server uses `--token`) |
| `--name NAME` | `Prometheus` | Display name at the table |
| `--selftest` | — | Run built-in correctness tests and exit |

### Quick start (cash game)

```bash
# 1. Start the server (headless, no GUI needed)
./server/build/server --headless --port 9000

# 2. Connect Prometheus
./bot/prometheus/build/prometheus --port 9000

# 3. Optionally add opponents for testing
./bot/titan/build/titan --port 9000 --name Titan
./bot/example/build/example_bot --port 9000 --name Bob
```

### Tournament

```bash
# 6-seat tournament; Prometheus fills one seat
./server/build/server --headless --tournament --max-players 6 --port 9000
./bot/prometheus/build/prometheus --port 9000
# … add more bots or the GUI client for the remaining seats
```

### Self-test

```bash
./bot/prometheus/build/prometheus --selftest
```

Runs 21 built-in checks covering range membership, equity accuracy, board texture classification, and hand-strength categorization. All tests must pass before connecting to a live server.

---

## Strategy Overview

Prometheus is a single-file C++ bot (~1 750 lines). Its strategy is organized into five layers.

### 1. Preflop — GTO-calibrated ranges with mixed strategies

Position-based open, 3-bet, and call ranges cover every seat in a 2–10 player game. Unlike a pure-strategy bot, Prometheus plays **mixed strategies** on its bluff combos: for example, A5s vs a late-position open gets 3-bet 55 % of the time and flatted 45 %, preventing opponents from exploiting a predictable range.

```
Open ranges:  BTN (widest) → CO → MP → EP → SB (each tuned separately)
3-bet ranges: value core + bluff hands at configurable frequency
BB defense:   3-bet range + wide call range (position-aware)
```

### 2. Short-stack / Tournament — Nash push/fold tables

When effective stack ≤ 20 BB, Prometheus switches to push/fold mode using **21 pre-built Nash equilibrium ranges** (one per BB depth from 1 BB to 20 BB). These are tighter and more accurate than the simple equity-threshold approach used by most bots.

### 3. Opponent modeling

Prometheus builds a profile for every opponent at the table. Stats are updated after every action:

| Stat | What it tracks |
|------|---------------|
| **VPIP** | Voluntarily put chips in preflop |
| **PFR** | Preflop raise frequency |
| **AF** | Aggression factor (raises / (calls + checks)) |
| **Fold to C-bet** | How often they fold to continuation bets |

Once enough hands are observed, Prometheus deviates exploitatively:

| Opponent profile | Adjustment |
|---|---|
| Nit (VPIP < 18 %) | Tighten call/3-bet ranges; respect their betting lines |
| Fish (loose/passive) | Value bet thin; drastically reduce bluff frequency |
| Aggro (AF > 2.5) | Trap with strong hands; widen calling range |
| Fold-happy (fold-to-cbet > 60 %) | C-bet nearly every board |

### 4. Postflop — board texture + hand classification

Every postflop decision starts by classifying the board:

| Texture class | Examples | Effect |
|---|---|---|
| **Monotone** | K♠Q♠J♠ | Large bets to charge flush draws |
| **Dynamic / wet** | 9♥8♣7♠ | Large bets; semi-bluff draws aggressively |
| **Static / dry** | K♦7♥2♣ | Small c-bets; polarized bluffs |
| **Paired** | A♠A♦7♣ | Check-raise trap lines more common |

Hand strength is then mapped into seven tiers — from **AIR** to **MONSTER** — using both raw phevaluator rank and computed equity. The tier (not just equity) drives sizing and line selection.

### 5. Dynamic bet sizing

Four sizes are available, chosen by hand strength × board texture × stage × SPR:

| Label | Pot fraction | When |
|---|---|---|
| **Small** | 33 % | Dry boards, thin value, probing |
| **Medium** | 66 % | Standard value bets and semi-bluffs |
| **Large** | 100 % | Wet/dynamic boards; protecting strong hands |
| **Overbet** | 150 % | River polarization (nut hands and select bluffs) |

Stack-to-pot ratio (SPR) also shifts commitment thresholds: low SPR → jam for max EV; high SPR → build the pot gradually.

---

## Equity Engine

Prometheus uses `phevaluator` for 7-card hand evaluation and a two-mode equity calculator:

| Situation | Method | Samples |
|---|---|---|
| River (5 board cards) | Exact enumeration over all opponent hands | C(45,2) = 990 |
| Turn (4 board cards) | Exact enumeration over hands × river run-outs | — |
| Flop / preflop | Monte Carlo | 2 000 (flop), 1 500 (preflop) |
| Multiway | Monte Carlo vs each opponent independently | max(300, 1 500 / n_opp) |

---

## Architecture

```
bot/prometheus/
├── bot.cpp          ← entire bot in one file
│   ├── Range system (169-class bitmask, token parser)
│   ├── Equity engine (phevaluator MC + exact river/turn)
│   ├── Board texture classifier
│   ├── Hand strength classifier (7 tiers)
│   ├── Preflop strategy (GTO ranges + mixed frequencies)
│   ├── Nash push/fold tables (1–20 BB)
│   ├── Opponent model (per-seat VPIP/PFR/AF/fold stats)
│   ├── Postflop decision engine (SPR + sizing selection)
│   └── WebSocket transport (libwebsockets, reconnect, ticker thread)
└── Makefile         ← mirrors bot/titan/Makefile
```

The transport layer is adapted from `bot/example/bot.cpp` and is fully compatible with the project's [API.md](../../API.md) protocol. The bot handles all message types: `welcome`, `queued`, `state`, `your_turn`, `action`, `stage`, `hand_over`, `reply`, `error`, and all tournament broadcasts.

---

## Comparison with Titan

| Capability | Titan | **Prometheus** |
|---|---|---|
| Preflop strategy | Fixed pure strategies | GTO-calibrated **mixed strategies** |
| Short-stack push/fold | 5 equity buckets | **21 Nash equilibrium ranges** |
| Opponent modeling | None | **VPIP / PFR / AF / fold-to-cbet** per seat |
| Postflop bet sizing | Fixed percentages | **4 sizes** chosen by context |
| Board texture awareness | None | Monotone / dynamic / static / paired |
| River strategy | Equity-only | **Polarized overbets** |
| Semi-bluff raises | Rare | Position- and frequency-aware |
| Tournament ICM awareness | None | Short-stack Nash, blind-level detection |

---

## Protocol Compatibility

Prometheus speaks the full [API.md](../../API.md) WebSocket/JSON protocol:

- Connects as a standard bot client (`is_human` is not set)
- Sends `hello` on connect, queries `my_cards` on each `your_turn`
- Falls back to `check_or_fold` on `bet_too_small` / `illegal_action` errors
- Reconnects automatically if the server drops the connection
- Works in both **cash** and **tournament** modes, at any table size (2–10 seats)
