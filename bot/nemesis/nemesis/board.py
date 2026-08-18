"""
Board texture classifier.

Pure string-parsing module (no phevaluator dependency) that extracts the
structural properties of the community cards relevant to postflop strategy:
wetness, suitedness, connectivity, paired-ness, and Broadway presence.

Used by strategy.py to select bet sizing and thresholds in _decide_postflop.
"""

from __future__ import annotations
from dataclasses import dataclass, field

_RANK_ORDER = "23456789TJQKA"
_RANK_VAL = {r: i for i, r in enumerate(_RANK_ORDER)}


@dataclass
class BoardTexture:
    # Suitedness
    monotone: bool = False        # all 3+ board cards same suit (nut-flush live)
    two_tone: bool = False        # 2 of one suit on flop (flush draw possible)
    rainbow: bool = False         # all different suits on flop

    # Pairs
    paired: bool = False          # at least one rank appears twice
    double_paired: bool = False   # two different ranks paired
    trips_on_board: bool = False  # a rank appears three times

    # Connectivity (0 = totally disconnected, 3 = very connected)
    connectivity: int = 0

    # Straight draw danger
    straight_draw_possible: bool = False  # 3 cards within a 5-rank window

    # Flush draw danger
    flush_draw_possible: bool = False     # 2+ same suit on board (before river)

    # Broadway / high-card presence
    has_ace: bool = False
    has_broadway: bool = False    # any A/K/Q/J/T on board
    highest_rank: int = 0         # 0 (2) .. 12 (A)

    # Derived summaries (convenience for strategy.py)
    is_wet: bool = False          # many draws possible — small bets, more check-raises
    is_dry: bool = False          # few draws — larger bets, more straightforward


def classify(board: list[str]) -> BoardTexture:
    """Classify a board from a list of API-format card strings ('As', 'Td', etc.).
    Cards that are None/empty are silently skipped. Returns a BoardTexture
    with all fields populated. Works on flop (3 cards), turn (4) or river (5)."""
    cards = [c for c in board if c]
    bt = BoardTexture()
    if len(cards) < 3:
        return bt

    ranks = [_RANK_VAL[c[0]] for c in cards]
    suits = [c[1] for c in cards]
    n = len(cards)

    # ---- suit analysis ----
    from collections import Counter
    suit_counts = Counter(suits)
    max_suit_count = max(suit_counts.values())

    bt.monotone = (max_suit_count == n and n == 3)
    bt.two_tone = (max_suit_count == 2 and n == 3)
    bt.rainbow = (max_suit_count == 1 and n == 3)

    # Flush draw: 2+ of same suit on any board (pre-river)
    bt.flush_draw_possible = max_suit_count >= 2

    # ---- rank analysis ----
    rank_counts = Counter(ranks)
    pairs = sum(1 for c in rank_counts.values() if c == 2)
    trips = sum(1 for c in rank_counts.values() if c >= 3)

    bt.paired = (pairs > 0 or trips > 0)
    bt.double_paired = (pairs >= 2)
    bt.trips_on_board = (trips > 0)

    # ---- high-card presence ----
    bt.highest_rank = max(ranks)
    bt.has_ace = (12 in ranks)
    bt.has_broadway = any(r >= 8 for r in ranks)  # T=8..A=12

    # ---- connectivity ----
    sorted_ranks = sorted(set(ranks))
    gaps = [sorted_ranks[i+1] - sorted_ranks[i] for i in range(len(sorted_ranks)-1)]
    small_gaps = sum(1 for g in gaps if g <= 4)
    bt.connectivity = min(3, small_gaps)

    # ---- straight draw possible ----
    # Check if any 5-rank window contains at least 3 of the board's distinct ranks
    distinct = sorted(set(ranks))
    bt.straight_draw_possible = False
    for lo in range(0, 9):  # windows 2-6, 3-7, ... T-A
        hi = lo + 4
        count = sum(1 for r in distinct if lo <= r <= hi)
        if count >= 3:
            bt.straight_draw_possible = True
            break

    # ---- wet / dry summaries ----
    draw_danger = int(bt.flush_draw_possible) + int(bt.straight_draw_possible)
    bt.is_wet = draw_danger >= 2 and not bt.paired
    bt.is_dry = draw_danger == 0 and not bt.monotone

    return bt
