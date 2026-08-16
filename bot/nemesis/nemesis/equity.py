"""
Equity engine. Uses eval7 (pip install eval7) for real Monte Carlo equity
against a random-hand or range-weighted opponent. Falls back to a crude
heuristic if eval7 isn't installed, so the bot never crashes -- but you
should install eval7 for real play (it's a single pip install, see
requirements.txt).
"""

from __future__ import annotations
import random

try:
    import eval7
    _HAVE_EVAL7 = True
except ImportError:
    _HAVE_EVAL7 = False


def _to_eval7_card(c: str):
    # API cards are like "As", "Td", "2c" -- eval7 wants the same rank/suit
    # letters lowercase, e.g. "As", "Td", "2c" (eval7.Card accepts this form).
    return eval7.Card(c)


def hand_equity_mc(hole: list[str], board: list[str], n_opponents: int,
                    samples: int = 1500) -> float:
    """Monte Carlo equity of `hole` vs `n_opponents` random hands, given the
    already-revealed `board` cards (list may contain fewer than 5)."""
    if not _HAVE_EVAL7:
        return _heuristic_equity(hole, board)

    board = [c for c in board if c]
    known = set(hole) | set(board)
    deck = [c for c in eval7.Deck().cards if str(c) not in known]

    wins = 0.0
    for _ in range(samples):
        random.shuffle(deck)
        draw = deck[: (5 - len(board)) + 2 * n_opponents]
        rest_board = draw[: 5 - len(board)]
        full_board = board + [str(c) for c in rest_board]
        opp_cards = draw[5 - len(board):]

        my_cards = [_to_eval7_card(c) for c in hole] + [_to_eval7_card(c) for c in full_board]
        my_score = eval7.evaluate(my_cards)

        best_opp = -1
        tie = False
        for i in range(n_opponents):
            oc = opp_cards[2 * i: 2 * i + 2]
            opp_cards_full = list(oc) + [_to_eval7_card(c) for c in full_board]
            s = eval7.evaluate(opp_cards_full)
            if s > best_opp:
                best_opp = s
                tie = False
            elif s == best_opp:
                tie = True

        if my_score > best_opp:
            wins += 1.0
        elif my_score == best_opp:
            wins += 1.0 / (2 if tie else 1)  # rough split handling
    return wins / samples


_RANK_VAL = {r: i for i, r in enumerate("23456789TJQKA", start=2)}


def _heuristic_equity(hole: list[str], board: list[str]) -> float:
    """Very rough fallback if eval7 isn't installed -- Chen-formula-flavored,
    just enough to keep the bot functional. Install eval7 for real play."""
    r1, r2 = hole[0][0], hole[1][0]
    suited = hole[0][1] == hole[1][1]
    v1, v2 = _RANK_VAL[r1], _RANK_VAL[r2]
    hi, lo = max(v1, v2), min(v1, v2)
    score = hi / 14.0 * 0.5 + (0.5 if v1 == v2 else 0.0) + (0.08 if suited else 0.0)
    score += max(0, 5 - (hi - lo)) * 0.01
    return min(0.95, max(0.05, score))
