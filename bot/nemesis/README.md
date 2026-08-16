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
main.py                    entry point / CLI
nemesis/
  protocol.py               WebSocket client, message dispatch, opponent-stat collection
  state.py                  parses server 'state' payloads; robust position assignment
  ranges.py                 169-hand grid, position-aware push/fold + open ranges
  icm.py                    winner-take-all ICM (win-probability) model
  equity.py                 Monte Carlo equity via eval7 (+ fallback heuristic)
  opponent_model.py         VPIP/PFR/AF/fold-to-cbet stats, Prometheus-fingerprint detector
  strategy.py                decision engine (preflop push/fold+opens, simple postflop)
tests/
  test_offline.py            unit tests that don't need a live server (ranges/state/icm math)
```

## Status

- **Offline unit tests**: `python3 tests/test_offline.py` — all pass (range
  widening by position, 15-20bb band not collapsed, ICM math, position
  assignment including with busted/empty seats, equity sanity checks).
- **Live-tested**: built the actual pokerish server + Prometheus + Titan +
  example bots from source and ran a real 9-max tournament with Nemesis
  filling the 9th seat. Confirmed via logs: correct handshake, correct
  position labeling every hand, push/fold shoves at short stack, deep-stack
  opens/3-bets/re-raises, and multi-street postflop equity-based decisions
  (check/call/bet/raise), with **zero protocol errors and zero crashes**
  across the full tournament (it completed with a winner declared).
- **Not yet done** (see the original phased plan): the opponent-model
  "looks like Prometheus" auto-detection is wired but not yet driving live
  strategy switches; postflop is intentionally a simple equity/pot-odds
  engine, not the board-texture-tiered engine from the plan's Phase 5;
  no large-sample (20k+ hand) win-rate benchmark has been run yet — that's
  the natural next step before trusting this in a real tournament.

## Next steps

1. Run a large-sample match (thousands of hands, `--tournament-end restart`
   in a loop, or many parallel tournaments) to get a real bb/100 or
   ROI-vs-Prometheus number — a handful of hands proves the plumbing works,
   not that it's actually profitable.
2. Wire `OpponentModel.looks_position_blind()` into `strategy.py` so the
   15-20bb exploit widens specifically against seats that show the
   Prometheus fingerprint, rather than always.
3. Build out the board-texture/range-advantage postflop engine (Phase 5 of
   the original plan) — current postflop is deliberately simple.
