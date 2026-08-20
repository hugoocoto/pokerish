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


# ---------------------------------------------------------------------------
# Hand key conversion (mirrors ranges.hand_key without creating a circular
# import): maps two card strings to a 169-grid canonical key like "AKs", "72o"
# ---------------------------------------------------------------------------
_RANKS = "23456789TJQKA"

def _card_to_hand_key(c1: str, c2: str) -> str:
    """Convert two API card strings to a 169-grid hand-class key."""
    r1, s1 = c1[0], c1[1]
    r2, s2 = c2[0], c2[1]
    if r1 == r2:
        return r1 + r2  # pair
    i1, i2 = _RANKS.index(r1), _RANKS.index(r2)
    hi, lo = (r1, r2) if i1 > i2 else (r2, r1)
    return hi + lo + ("s" if s1 == s2 else "o")


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

        # Collect all opponent scores in one pass so we can correctly handle
        # three-way chops and cases where opponents tie each other (not us).
        opp_scores = []
        for i in range(n_opponents):
            oc = opp_cards[2 * i: 2 * i + 2]
            opp_full = list(oc) + [_to_eval7_card(c) for c in full_board]
            opp_scores.append(eval7.evaluate(opp_full))

        best_opp = max(opp_scores) if opp_scores else -1
        if my_score > best_opp:
            wins += 1.0
        elif my_score == best_opp:
            # We tied the best opponent.  Count how many players share the winning
            # score (us + all opponents at that score) to split correctly.
            tied_count = 1 + sum(1 for s in opp_scores if s == best_opp)
            wins += 1.0 / tied_count
        # else: my_score < best_opp → we lose, add nothing
    return wins / samples


def hand_equity_vs_range(hole: list[str], board: list[str],
                          opp_range: set[str],
                          samples: int = 1200) -> float | None:
    """Monte Carlo equity of `hole` against a *specific opponent range* via
    rejection sampling.  Only opponent hands that appear in `opp_range` are
    accepted; all others are discarded and redrawn.

    This is the key upgrade over Titan/Prometheus: instead of computing equity
    against a completely random hand (which assumes the opponent shoves 100%),
    we model their actual distribution. Against a nit's range (top 8%) our
    hand has much less equity than against a fish's range (top 60%). Using
    the wrong model causes systematic mis-calls in both directions.

    Returns None when:
      - eval7 is not installed
      - the opponent range is empty
      - too few accepted samples (range too narrow for the deck state)
    The caller should fall back to hand_equity_mc in these cases.
    """
    if not _HAVE_EVAL7 or not opp_range:
        return None

    board = [c for c in board if c]
    known = set(hole) | set(board)
    deck_strs = [str(c) for c in eval7.Deck().cards if str(c) not in known]
    n = len(deck_strs)
    need = 5 - len(board)  # board cards still to be dealt

    if n < 2 + need:
        return None

    wins = 0.0
    accepted = 0
    attempts = 0
    # Allow up to 30× rejections before giving up (handles ranges as tight as 3%)
    max_attempts = samples * 30

    while accepted < samples and attempts < max_attempts:
        attempts += 1
        # Sample 2 opponent cards + `need` board cards in one shot (no replacement).
        indices = random.sample(range(n), 2 + need)
        c1_str = deck_strs[indices[0]]
        c2_str = deck_strs[indices[1]]
        key = _card_to_hand_key(c1_str, c2_str)
        if key not in opp_range:
            continue  # reject: opponent wouldn't shove this hand

        board_fill = [deck_strs[indices[2 + k]] for k in range(need)]
        full_board = board + board_fill

        my_cards = [_to_eval7_card(c) for c in hole] + [_to_eval7_card(c) for c in full_board]
        my_score = eval7.evaluate(my_cards)

        opp_cards = [_to_eval7_card(c1_str), _to_eval7_card(c2_str)] + \
                    [_to_eval7_card(c) for c in full_board]
        opp_score = eval7.evaluate(opp_cards)

        if my_score > opp_score:
            wins += 1.0
        elif my_score == opp_score:
            wins += 0.5
        accepted += 1

    # Fall back if too few draws hit the range (avoids noisy results from tiny ranges)
    if accepted < samples // 5:
        return None
    return wins / accepted


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


