# Nemesis

A 9-max pokerish tournament bot with fundamentally sound, position-aware
preflop and board-texture-aware postflop play, plus an adaptive layer that
watches every opponent's actual observed behavior and exploits specific
patterns wherever it finds them — it does not assume anything about who's
sitting in the other seats.

Core strategy:

1. Push/fold ranges are indexed by `(position, stack_bb)`, not stack depth
   alone — a real Nash-shaped chart varies a lot by seat at the same stack.
2. Calling ranges stay populated smoothly across the whole 1-20bb band
   instead of having a dead zone at any particular depth.
3. A real (if simple, appropriate-for-winner-take-all) ICM layer lives in
   `nemesis/icm.py` — win-probability-based risk premiums, not just a claim.
4. Facing a raise, Nemesis uses full 9-position granularity instead of
   collapsing raiser positions into a handful of buckets.
5. All-in calls are pot-odds based with equity computed against the
   opponent's *estimated shove range* (profile + position + fingerprint
   adjusted), not against a random hand — a 10x overbet gets folded where a
   2x-pot shove gets called.
6. When we were the preflop raiser, postflop c-bet logic uses range
   advantage rather than raw equity; a 4-bet/stackoff response handles
   3-bet pots, and the BB iso-raises limpets instead of checking blindly.

On top of that baseline, `opponent_model.py` fingerprints specific leaks in
whoever it's playing against — e.g. a seat whose shove frequency barely
varies by position gets its calling/3-bet range widened specifically against
that seat, once enough hands confirm the pattern. This adapts automatically:
against an opponent that doesn't show the pattern, nothing changes.

This has been build-tested and **live-tested against the real compiled
Prometheus, Titan, and example bots** on the actual pokerish server —
see "Status" below.

## Install

```bash
pip install -r requirements.txt
```

(`eval7` is the equity engine; if it fails to install for some reason the
bot falls back to a crude heuristic equity function so it still runs, but
install eval7 for real play.)

## Run

Point it at a running pokerish server (see the main pokerish repo's
`HOW_TO_HOST.md` for starting the server, e.g.
`./server/build/server --headless --tournament --max-players 9`):

```bash
python3 bot.py --host 127.0.0.1 --port 9000 --name Nemesis [--token SECRET] [-v]
```

## Project layout

```
bot.py                     entry point / CLI
nemesis/
  protocol.py               WebSocket client, message dispatch, opponent-stat collection
  state.py                  parses server 'state' payloads; robust position assignment
  ranges.py                 169-hand grid, position-aware push/fold + open ranges
  icm.py                    winner-take-all ICM (win-probability) model
  equity.py                 Monte Carlo equity via eval7 (+ fallback heuristic)
  opponent_model.py         VPIP/PFR/AF/fold-to-cbet stats, position-blind fingerprint detector
  board.py                  board texture classifier (wet/dry/paired/monotone/connectivity)
  strategy.py               decision engine (preflop push/fold+opens, board-texture postflop)
tests/
  test_offline.py            unit tests that don't need a live server (ranges/state/icm/board/strategy)
```

## Status

- **Offline unit tests**: `python3 tests/test_offline.py` — all pass (range
  widening by position, 15-20bb band not collapsed, ICM math, position
  assignment including with busted/empty seats, equity sanity checks, board
  texture classification, fingerprint detection, range-weighted pot-odds
  calls, 4-bet/stackoff, BB iso-raise, c-bet, stage-aware ICM, and
  strategy-level exploit verification).
- **Live-tested**: built the actual pokerish server + Prometheus + Titan +
  example bots from source and ran a real 9-max tournament with Nemesis
  filling the 9th seat. Confirmed via logs: correct handshake, correct
  position labeling every hand, push/fold shoves at short stack, deep-stack
  opens/3-bets/re-raises, and multi-street postflop equity-based decisions
  (check/call/bet/raise), with **zero protocol errors and zero crashes**
  across the full tournament (it completed with a winner declared).

### Completed features

1. **Position-aware push/fold ranges** — every seat plays a distinct,
   correctly-shaped range at a given stack depth.
2. **15-20bb calling gap filled** — no dead zone anywhere in the push/fold
   band.
3. **Real winner-take-all ICM layer** — win-probability-based risk premium,
   stage-aware (lenient on the flop, tightest on the river), not just a
   claim.
4. **Full 9-position 3-bet/call matrix** — every raiser seat gets a distinct
   continuation frequency.
5. **Adaptive fingerprint exploit** — `OpponentModel.looks_position_blind()`
   populates from broadcast actions (shove opportunities recorded and
   flushed at hand end); `strategy.py` widens calling/shoving/3-bet ranges
   specifically against whichever seat(s) actually show the pattern, at
   any stack depth, with zero effect against opponents that don't.
6. **Board-texture-aware postflop** — `board.py` classifies wet/dry/paired/
   monotone/connectivity; `strategy.py` uses it to select sizing (larger on
   dry, smaller on wet), tightened multi-way thresholds, check-trap on paired
   boards, and semi-bluff frequency tuned per texture.
7. **Range-weighted pot-odds calls** — short-stack all-in decisions use real
   pot odds plus equity against the opponent's estimated shove range
   (Nash shape, adjusted by profile and fingerprint), with an ICM margin on
   top; the same hand can correctly fold to an overbet but call a smaller
   shove.
8. **3-bet pot handling** — as the preflop raiser facing a re-raise: 4-bet
   jam with the stackoff range (AA/KK/AK), blocker 4-bet bluffs (A5s-A3s) at
   frequency, flat with a call range, else fold.
9. **BB iso-raise vs limpers** — punish passive open-limpers with a
   sizing-adjusted isolation raise from the big blind.
10. **PFR-aware c-betting** — when we raised preflop, bet with range
    advantage on dry boards and pure-bluff c-bet at low frequency instead of
    requiring value equity.
11. **Position-aware postflop** — OOP thresholds tightened for value, raises
    and semi-bluffs; IP play more freely.
12. **Aggregated opponent reads** — postflop exploit decisions (e.g. the
    fold-happy bluff bonus) aggregate across *all* live opponents: a single
    calling station in the pot shuts the exploit down, and a bluff only gets
    credit if it can get through everyone.

## Run

Point at a running pokerish server (see main repo's `HOW_TO_HOST.md`):

```bash
python3 -m venv venv
venv/bin/pip install -r requirements.txt
venv/bin/python bot.py --host 127.0.0.1 --port 9000 --name Nemesis [--token SECRET] [-v]
```

Or use the convenience scripts (build everything, spin up a tournament that
includes Nemesis — watch it on the GUI server, or play yourself via the
headless variant):

```bash
scripts/run_tournament.sh [N]        # GUI server, bot-only, spectate
scripts/run_headless.sh [N]          # headless server + your GUI client
```