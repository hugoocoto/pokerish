# Hosting a Poker Game or Tournament

This guide explains how to run the poker server and what every flag does. The
server is one binary: game engine, WebSocket server and optional raylib table
GUI. Players connect over WebSockets using the protocol in
[API.md](file:///home/hugo/code/poker/API.md).

---

## 1. Build

```sh
make -j4          # builds server/build/server, bot/example/build/example_bot,
                  # client/build/client and bot/titan/build/titan
```

First build compiles the third-party submodules (raylib takes minutes). The
GUI backend follows `RAYLIB_PLATFORM` (auto-detected from the session on
Linux: `wayland` or `x11`; use `make RAYLIB_PLATFORM=x11` to force one). On a
headless box use `--headless`.

## 2. The two game modes

**Cash mode** (default): 2–10 seats. Empty seats are filled by instant auto
bots; a human or bot client that says `hello` queues for a seat and takes it
over at the end of the current round. Players who lose all chips are rebought
to the start stack, so the game never ends. Hands chain instantly (no pause
between hands).

**Tournament mode** (`--tournament`): no bots. Clients seat immediately in
the lobby, a countdown runs once the table is full, then blind levels rise on
a schedule (section 6). Eliminated players are gone (no rebuy); the last
player wins everything. What happens after the winner is declared is chosen
with `--tournament-end` (default: stay on the winner screen).

## 3. Quick start

```sh
# Cash game on the LAN, human-pace "thinking" delays:
./server/build/server --host 0.0.0.0 --simulate

# Connect players from other machines:
./bot/example/build/example_bot --host 192.168.1.20
./client/build/client            --host 192.168.1.20   # raylib play client

# 4-player tournament, 2-minute levels, restart automatically after it ends:
./server/build/server --tournament --max-players 4 --level-seconds 120 \
                      --tournament-end restart
```

The server has its own raylib table GUI showing every card (like a croupier
view). `--headless` runs the same server without it.

## 4. Server flags

| Flag | Default | Meaning |
|------|---------|---------|
| `--port N` | 9000 | TCP port to listen on (1–65535). |
| `--host IP` | 127.0.0.1 | Interface to bind. Use `0.0.0.0` for LAN. |
| `--token SECRET` | none | Require clients to send `SECRET` in their `hello`, else they are rejected as unauthorized. |
| `--headless` | off | Run without the raylib GUI (server-side log only). |
| `--max-players N` | 6 | Seats, 2–10 (default 6; alias `--table-size`). Applies to both modes. |
| `--action-timeout N` | 20 | Seconds a player has to act before the engine folds them (min 1). |
| `--start-stack N` | 1000 | Entry stack (cash rebuys use it too). |
| `--simulate [MAX]` | off | Hold every action for a uniform random 1.0..MAX s "thinking" delay before it takes effect (bare `--simulate` → 3.0; MAX must be ≥ 1.0). A player who already answered is never folded on timeout — only players who never respond fold. |
| `--hand-pause N` | 0 / 3.0 | Pause between hands. Without `--simulate` it is always 0 (hands chain instantly) and this flag is an error; with `--simulate` it defaults to 3.0 and `--hand-pause N` (≥ 0) overrides it. |
| `--export-dir DIR` | hands | Directory for PokerStars-format hand history (alias `--hand-history`). |
| `--tournament` | off | Tournament mode (below). |
| `--level-seconds N` | 300 | Seconds per blind level (min 1). |
| `--countdown-seconds N` | 10 | Countdown after the table fills before the first hand (min 0). |
| `--tournament-end MODE` | stay | What happens when the winner is declared: `stay`, `exit` or `restart` (below). Requires `--tournament`. |
| `--tournament-end-hold N` | 5 | Seconds to hold the winner screen with `--tournament-end restart` (min 0). Error unless used with `restart`. |
| `--scale N` | auto | GUI scale level, 0.5–2.0, where 2.0 is the current look (cards 96×128 at 1280×720). Without it the GUI picks the largest of {2.0, 1.5, 1.25, 1.0, 0.75, 0.5} that fits the window, re-fitting on resize. Same flag on the client. |

Run `./server/build/server` with no arguments to print the usage line.

## 5. Clients

| Binary | Use |
|--------|-----|
| `bot/example/build/example_bot --port N [--host IP] [--token SECRET] [--name NAME]` | Minimal C++ bot that plays random-ish hands instantly (person-style random name like `Bob`). |
| `bot/python/bot.py` (via `bot/python/venv/bin/python`) | Same bot in Python, same flags. |
| `bot/titan/build/titan` | Stronger bot: position-based preflop ranges, Monte-Carlo equity, pot-odds and short-stack push/fold. Same flags plus `--selftest`. |
| `client/build/client` | raylib play client for a human: your hole cards face up, Fold/Call/Raise buttons. Same flags plus `--scale N` (see the server table). |

In cash mode a client queues for the first free seat and takes it over when
the current round ends. In tournament mode clients take their seats
immediately in the lobby/countdown; once the tournament is running no late
joins are accepted.

## 6. Tournament details

- **Schedule** — levels of 1..12 with antes from level 5; past level 12 the
  blinds keep doubling, so a tournament always ends (starting stack 1000 =
  100 BB, 6000 chips in play):

  | Level | SB | BB | Ante |
  |-------|----|----|------|
  | 1  | 5    | 10    | –    |
  | 2  | 10   | 20    | –    |
  | 3  | 15   | 30    | –    |
  | 4  | 25   | 50    | –    |
  | 5  | 40   | 80    | 5    |
  | 6  | 60   | 120   | 10   |
  | 7  | 100  | 200   | 15   |
  | 8  | 150  | 300   | 25   |
  | 9  | 250  | 500   | 40   |
  | 10 | 400  | 800   | 60   |
  | 11 | 600  | 1200  | 100  |
  | 12 | 1000 | 2000  | 150  |

- **Flow**: lobby (waiting for players) → countdown once the table is full →
  running (hands dealt, blinds rise) → finished (one player left). The winner
  takes the whole stack; `tournament_over` is broadcast.
- **Disconnects**: a player who disconnects mid-hand is folded immediately
  (their committed chips stay in the pot) and eliminated at the round end.
- **`--tournament-end stay`** (default): the server stays on the winner
  screen; new `hello`s are rejected. Restart the server to play again.
- **`--tournament-end exit`**: the server stops when the winner is declared
  (the GUI window closes; `run()` returns).
- **`--tournament-end restart`**: everyone is kicked and the table resets to
  a fresh lobby after `--tournament-end-hold N` seconds on the winner screen.
  Clients must say `hello` again — the included bots reconnect and rejoin
  automatically.

## 7. Examples

```sh
# Private cash game, only for you and your friends:
./server/build/server --port 9000 --token hunter2

# LAN game with 4 seats and a 10-second action clock:
./server/build/server --host 0.0.0.0 --max-players 4 --action-timeout 10

# Same, but bots visibly "think" 1–2 s before acting:
./server/build/server --host 0.0.0.0 --max-players 4 --simulate 2.0 --hand-pause 1.0

# 6-max sit-and-go, 2-minute levels, auto-restart for the next one:
./server/build/server --tournament --level-seconds 120 --countdown-seconds 15 \
                      --tournament-end restart --tournament-end-hold 10

# Headless tournament that shuts the server down at the winner:
./server/build/server --headless --tournament --tournament-end exit
```

`scripts/run_tournament.sh` does all of this for you: builds everything, starts a
6-player tournament server in the background, launches 1 Titan + 2 C++ + 2
Python bots, and connects your human play client in the foreground. A bot-only
variant is `scripts/run_bot_tournament.sh` (4-seat tournament, one of each bot,
no human client).
