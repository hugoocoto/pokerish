"""
Position-aware hand ranges.

This is Nemesis's answer to Prometheus's biggest confirmed leak: Prometheus's
push_range[1..20] is indexed ONLY by stack depth, identical from every seat.
Real Nash push/fold ranges vary a lot by position (UTG at 10bb shoves far
tighter than the button at 10bb). Here, ranges are indexed by
(position, stack_depth_bb) so Nemesis plays a different, correct-shaped range
from every one of the 9 seats.

The percentages below are a principled approximation built from well known,
publicly documented push/fold theory (the general shape of Nash/ICM shove
charts: tight under the gun, exponentially widening toward the button and
blinds, narrowing again as effective stacks grow past ~15-20bb). They are not
a from-scratch CFR solve -- treat them as a strong, structurally-correct
starting point that already fixes Prometheus's two confirmed leaks (position
blindness, and the 15-20bb under-calling gap), and swap in real solver output
later if you compute one (see PLAN Phase 2).
"""

from __future__ import annotations
from dataclasses import dataclass
from itertools import product

RANKS = "23456789TJQKA"  # low to high


def _hand_score(r1: str, r2: str, suited: bool) -> float:
    """Rough all-in-equity-flavored score used only to ORDER the 169 hands,
    not to assign literal equities. Higher = stronger for push/fold purposes."""
    i1, i2 = RANKS.index(r1), RANKS.index(r2)
    hi, lo = max(i1, i2), min(i1, i2)
    if r1 == r2:  # pocket pair
        return 200 + hi * 10
    score = hi * 6 + lo * 2
    if suited:
        score += 8
    gap = hi - lo
    # connectors / small gaps play better in shove-heavy spots (more equity
    # vs. calling ranges, more playability if called and it's not all-in)
    score += max(0, 5 - gap) * 1.5
    # ace-x and king-x get a small blocker/domination bonus
    if hi == 12:  # ace
        score += 4
    return score


def _all_169() -> list[str]:
    hands = []
    for i, r1 in enumerate(RANKS):
        for j, r2 in enumerate(RANKS):
            if i == j:
                hands.append(r1 + r2)  # pair, e.g. "AA"
            elif i > j:
                hands.append(r1 + r2 + "s")
            else:
                pass
    for i, r1 in enumerate(RANKS):
        for j, r2 in enumerate(RANKS):
            if i > j:
                hands.append(r1 + r2 + "o")
    return hands


def _canonical_order() -> list[str]:
    hands = _all_169()

    def score_of(h: str) -> float:
        if len(h) == 2:
            return _hand_score(h[0], h[1], suited=False)  # pair
        r1, r2, kind = h[0], h[1], h[2]
        return _hand_score(r1, r2, suited=(kind == "s"))

    return sorted(hands, key=score_of, reverse=True)


CANONICAL_ORDER = _canonical_order()  # index 0 = strongest (AA), 168 = weakest (72o)
assert len(CANONICAL_ORDER) == 169, len(CANONICAL_ORDER)
_RANK_INDEX = {h: i for i, h in enumerate(CANONICAL_ORDER)}


def hand_key(card1: str, card2: str) -> str:
    """Convert two API-format cards ('As','Kd') into a 169-grid key like 'AKs'/'AKo'/'AA'."""
    r1, s1 = card1[0], card1[1]
    r2, s2 = card2[0], card2[1]
    i1, i2 = RANKS.index(r1), RANKS.index(r2)
    if r1 == r2:
        return r1 + r2
    hi, lo = (r1, r2) if i1 > i2 else (r2, r1)
    return hi + lo + ("s" if s1 == s2 else "o")


def top_pct(pct: float) -> set[str]:
    """Return the top `pct` percent of the 169-hand grid, weighted by combo count
    (pairs = 6 combos, suited = 4, offsuit = 12) so the returned SET represents
    the correct percentage of actual 1326 starting hand combos, not just of the
    169 canonical classes."""
    pct = max(0.0, min(100.0, pct))
    target_combos = pct / 100.0 * 1326
    combos = 0
    out = set()
    for h in CANONICAL_ORDER:
        c = 6 if len(h) == 2 else (4 if h[2] == "s" else 12)
        if combos >= target_combos:
            break
        out.add(h)
        combos += c
    return out


# ---------------------------------------------------------------------------
# Position model: 9-max seats, 0 = UTG (first to act preflop) ... 8 = BB
# ---------------------------------------------------------------------------
POSITIONS_9MAX = ["UTG", "UTG1", "MP1", "MP2", "HJ", "CO", "BTN", "SB", "BB"]

# Base push (shove) percentage AT 10BB EFFECTIVE, unopened pot, by position.
# Shape: tight early, ~doubling by the hijack, wide on the button/blinds.
# (General published push/fold theory shape, not Prometheus-derived.)
BASE_PUSH_PCT_10BB = {
    "UTG": 9.0, "UTG1": 11.0, "MP1": 14.0, "MP2": 17.0,
    "HJ": 21.0, "CO": 27.0, "BTN": 38.0, "SB": 52.0, "BB": 100.0,  # BB never "pushes" unopened
}


def _depth_factor(stack_bb: float) -> float:
    """Multiplier applied to the 10bb base rate as effective stack changes.
    >1 for shorter stacks (wider), <1 approaching 20bb (tighter), and
    critically NEVER collapses to near-zero in the 15-20bb band the way
    Prometheus effectively does -- that's exploit target #2 from the plan."""
    stack_bb = max(1.0, min(20.0, stack_bb))
    # smooth exponential: f(10) == 1.0 by construction
    import math
    return math.exp(-0.085 * (stack_bb - 10.0))


def push_range(position: str, stack_bb: float) -> set[str]:
    base = BASE_PUSH_PCT_10BB.get(position, 20.0)
    pct = min(100.0, base * _depth_factor(stack_bb))
    return top_pct(pct)


def call_shove_range(position: str, stack_bb: float, num_extra_risk: int = 0) -> set[str]:
    """Range to CALL an all-in with (not shove ourselves). Tighter than the
    matching push range because we need enough equity to overcome the caller's
    disadvantage (they can't fold out worse hands the way a shove can).
    `num_extra_risk` > 0 tightens further (used for ICM risk premium)."""
    base = BASE_PUSH_PCT_10BB.get(position, 20.0) * 0.58
    pct = min(100.0, base * _depth_factor(stack_bb))
    pct *= max(0.55, 1.0 - 0.08 * num_extra_risk)
    return top_pct(pct)


# ---------------------------------------------------------------------------
# Deep-stack (>20bb) preflop opens, per position. Percent of hands opened
# first-in. 3-bet ranges are a fixed fraction of the opener's range.
# ---------------------------------------------------------------------------
OPEN_PCT = {
    "UTG": 12.0, "UTG1": 14.0, "MP1": 16.0, "MP2": 18.0,
    "HJ": 21.0, "CO": 26.0, "BTN": 42.0, "SB": 38.0,
}


def open_range(position: str) -> set[str]:
    return top_pct(OPEN_PCT.get(position, 18.0))


def threebet_value_range(position: str) -> set[str]:
    # tight, position-independent value core; bluffs handled separately in strategy.py
    return top_pct(4.0 if position in ("UTG", "UTG1", "MP1") else 6.0)


def call_open_range(position: str, opener_position_idx: int, my_position_idx: int) -> set[str]:
    """Flat-calling range vs. a single open, position aware: wider in position,
    tighter out of position."""
    ip = my_position_idx > opener_position_idx  # True = we're closing action / in position
    pct = 16.0 if ip else 9.0
    return top_pct(pct)
