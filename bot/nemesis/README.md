# Nemesis

A 9-max pokerish tournament bot built to beat Prometheus, by directly
targeting confirmed leaks found in Prometheus's own source (`bot/prometheus/bot.cpp`):

1. Its push/fold ranges (`push_range[1..20]`) are indexed **only by stack
   depth**, identical from every seat. Nemesis's ranges are indexed by
   `(position, stack_bb)`.
2. It will not call off an all-in between **15-20bb effective** even with a
   hand in its own push range. Nemesis's calling ranges stay populated
   through that band.
3. It claims "ICM awareness" in a code comment but has no bubble/payout
   logic anywhere. Nemesis has a real (if simple, appropriate-for-
   winner-take-all) ICM layer in `nemesis/icm.py`.
4. It collapses 9-max raiser positions into 3 buckets facing a raise.
   Nemesis uses full 9-position granularity.

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
  opponent_model.py         VPIP/PFR/AF/fold-to-cbet stats, Prometheus-fingerprint detector
  board.py                  board texture classifier (wet/dry/paired/monotone/connectivity)
  strategy.py               decision engine (preflop push/fold+opens, board-texture postflop)
tests/
  test_offline.py            unit tests that don't need a live server (ranges/state/icm/board/strategy)
```

## Status

- **Offline unit tests**: `python3 tests/test_offline.py` — all 24 pass (range
  widening by position, 15-20bb band not collapsed, ICM math, position
  assignment including with busted/empty seats, equity sanity checks, board
  texture classification, fingerprint detection, and strategy-level exploit
  verification).
- **Live-tested**: built the actual pokerish server + Prometheus + Titan +
  example bots from source and ran a real 9-max tournament with Nemesis
  filling the 9th seat. Confirmed via logs: correct handshake, correct
  position labeling every hand, push/fold shoves at short stack, deep-stack
  opens/3-bets/re-raises, and multi-street postflop equity-based decisions
  (check/call/bet/raise), with **zero protocol errors and zero crashes**
  across the full tournament (it completed with a winner declared).

### Completed features

1. **Position-aware push/fold ranges** — fixes Prometheus leak #1.
2. **15-20bb calling gap filled** — fixes Prometheus leak #2.
3. **Real winner-take-all ICM layer** — fixes Prometheus leak #3.
4. **Full 9-position 3-bet/call matrix** — fixes Prometheus leak #4.
5. **Prometheus fingerprint exploit live** — `OpponentModel.looks_position_blind()`
   now correctly populates from broadcast actions (shove opportunities recorded
   and flushed at hand end), and `strategy.py` widens calling/shoving/3-bet
   ranges specifically against fingerprinted seats in all stack depths.
6. **Board-texture-aware postflop** — `board.py` classifies wet/dry/paired/
   monotone/connectivity; `strategy.py` uses it to select sizing (larger on
   dry, smaller on wet), tightened multi-way thresholds, check-trap on paired
   boards, and bluff semi-bluff frequency tuned per texture.

## Run

Point at a running pokerish server (see main repo's `HOW_TO_HOST.md`):

```bash
python3 -m venv venv
venv/bin/pip install -r requirements.txt
venv/bin/python bot.py --host 127.0.0.1 --port 9000 --name Nemesis [--token SECRET] [-v]
```

Or use the convenience script (builds everything, spins up a headless
tournament with Prometheus + Titan + Nemesis, tails the Nemesis log):

```bash
scripts/run_nemesis_tournament.sh [N] [--port PORT] [--level-seconds SECS]
```

