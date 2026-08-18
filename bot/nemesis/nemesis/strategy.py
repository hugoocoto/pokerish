"""
Nemesis decision engine.

Preflop:
  - ≤20bb: position-aware Nash push/fold (fixes Prometheus's leak #1).
    Call ranges stay populated through the 15–20bb band (fixes leak #2).
    When a seat's shove fingerprint matches Prometheus's position-blind pattern
    (leak #1 confirmed), we widen call ranges specifically vs. that seat.
  - >20bb: position-granular opens, 9-position 3-bet/call matrix (fixes leak #4).
    Bluff 3-bet frequency adjusted by opponent profile; when the raiser is
    fingerprinted as position-blind, treat their range as ~15% looser.

Postflop:
  - Monte Carlo equity via eval7 (falls back to heuristic if not installed).
  - Board-texture-aware sizing: larger on dry boards, smaller on wet boards.
  - Multi-way pot thresholds tightened vs. single-opponent.
  - ICM chip-leader guard: avoid high-variance marginal spots when stack share > 35%.
  - Opponent profile adjustments: fold-happy opponents get more bluffs.
"""

from __future__ import annotations
import random
from . import ranges, icm, equity
from .board import classify as classify_board
from .state import GameState
from .opponent_model import OpponentModel


class Decision:
    def __init__(self, action: str, amount: int = 0):
        self.action = action  # 'fold' | 'check' | 'call' | 'call_all' | 'bet'
        self.amount = amount  # raise INCREMENT for 'bet' (see API.md)


def decide(gs: GameState, om: OpponentModel, my_seat: int, hole: list[str],
           last_raiser_seat: int | None = None) -> Decision:
    if gs.stage == "preflop":
        return _decide_preflop(gs, om, my_seat, hole, last_raiser_seat)
    return _decide_postflop(gs, om, my_seat, hole)


# ---------------------------------------------------------------------------
# Preflop
# ---------------------------------------------------------------------------

def _decide_preflop(gs: GameState, om: OpponentModel, my_seat: int, hole: list[str],
                    last_raiser_seat: int | None) -> Decision:
    key = ranges.hand_key(hole[0], hole[1])
    pos_map = gs.position_map()
    my_pos = pos_map.get(my_seat, "MP1")
    me = gs.players[my_seat]
    bb = gs.big_blind()
    stack_bb = gs.effective_stack_bb(my_seat)

    to_call = gs.current_bet - me.street_bet
    all_stacks = [p.stack + p.bet for p in gs.players.values() if not p.busted]

    facing_raise = gs.current_bet > bb and any(
        p.street_bet == gs.current_bet and p.seat != my_seat for p in gs.players.values()
    )
    is_allin_facing = to_call >= me.stack  # calling would commit our whole stack

    # Whether the raiser shows Prometheus's position-blind fingerprint
    raiser_is_fingerprinted = (
        last_raiser_seat is not None
        and om.get(last_raiser_seat).looks_position_blind()
    )

    # --- Short-stack push/fold zone (≤20bb effective) ---
    if stack_bb <= 20:
        if is_allin_facing and to_call > 0:
            raiser_stack = _find_raiser_stack(gs, last_raiser_seat, my_seat)
            risk = icm.icm_risk_premium(me.stack, raiser_stack, all_stacks) if raiser_stack else 0

            # Exploit: vs. a fingerprinted seat, their shove range is wider than
            # their position implies, so we can call tighter ICM risk (range is less
            # likely to dominate us; we have more raw equity).
            if raiser_is_fingerprinted:
                risk = max(0.0, risk - 1.0)  # tighten ICM penalty → widen call

            call_range = ranges.call_shove_range(my_pos, stack_bb, num_extra_risk=risk)
            if key in call_range:
                return Decision("call_all")
            return Decision("fold")

        if to_call == 0 or not facing_raise:
            push = ranges.push_range(my_pos, stack_bb)
            if key in push:
                return Decision("bet", me.stack)  # shove: amount caps at stack per API.md
            return Decision("check" if to_call == 0 else "fold")

        # facing a raise (not yet all-in for us) while short: treat as push/fold
        push = ranges.push_range(my_pos, stack_bb)
        if key in push:
            return Decision("bet", me.stack)

        # Extra: vs. a fingerprinted raiser in the 15-20bb zone, widen the shove-
        # over-a-raise range (their range is larger so our shove has more fold equity)
        if raiser_is_fingerprinted and stack_bb >= 15:
            wider_push = ranges.push_range(my_pos, max(1.0, stack_bb - 3))
            if key in wider_push:
                return Decision("bet", me.stack)

        return Decision("fold")

    # --- Deep stack (>20bb) ---
    if not facing_raise:
        opens = ranges.open_range(my_pos)
        if key in opens:
            size = int(2.5 * bb)
            return Decision("bet", size)
        return Decision("check" if to_call == 0 else "fold")

    # facing exactly one raise: build full 9-position raiser context
    raiser_seat = last_raiser_seat if last_raiser_seat is not None \
        else _guess_raiser_seat(gs, my_seat)
    raiser_pos = pos_map.get(raiser_seat, "MP1") if raiser_seat is not None else "MP1"
    raiser_pos_idx = ranges.POSITIONS_9MAX.index(raiser_pos) \
        if raiser_pos in ranges.POSITIONS_9MAX else 3
    my_pos_idx = ranges.POSITIONS_9MAX.index(my_pos) if my_pos in ranges.POSITIONS_9MAX else 3

    profile = om.classify(raiser_seat) if raiser_seat is not None else "unknown"
    value_3b = ranges.threebet_value_range(my_pos)

    # Bluff 3-bet frequency: base 25%, boosted vs. fold-happy / nit profiles
    bluff_3b_freq = 0.45 if profile in ("fold_happy", "nit") else 0.25
    # Boost further if the raiser is fingerprinted as position-blind (wide, easily
    # dominated by our 3-bet range)
    if raiser_is_fingerprinted:
        bluff_3b_freq = min(0.60, bluff_3b_freq + 0.15)

    if key in value_3b:
        size = int(3.2 * gs.current_bet) - gs.current_bet
        return Decision("bet", max(size, gs.min_raise))

    # Thin, position-differentiated bluff 3-bets
    if my_pos in ("CO", "BTN", "SB") and random.random() < bluff_3b_freq:
        thin_bluffs = ranges.top_pct(3.0) - value_3b
        if key in thin_bluffs:
            size = int(3.2 * gs.current_bet) - gs.current_bet
            return Decision("bet", max(size, gs.min_raise))

    # Flat call: use full 9-position granularity (fixes Prometheus leak #4).
    # Vs. a fingerprinted seat: treat their range as ~3-position-slots wider
    # (they open wider than their position implies) → we can call a bit wider too.
    call_pct_bonus = 3.0 if raiser_is_fingerprinted else 0.0
    calls = ranges.call_open_range(my_pos, raiser_pos_idx, my_pos_idx,
                                   extra_pct=call_pct_bonus)
    if key in calls:
        return Decision("call")
    return Decision("fold" if to_call > 0 else "check")


def _find_raiser_stack(gs: GameState, raiser_seat: int | None, my_seat: int) -> int | None:
    """Return total chips (stack + bet) of the aggressor we're facing, or None."""
    if raiser_seat is not None and raiser_seat in gs.players:
        p = gs.players[raiser_seat]
        return p.stack + p.bet
    # Fallback: find any non-self player still live with the current bet
    for s, p in gs.players.items():
        if s != my_seat and not p.folded and p.street_bet == gs.current_bet:
            return p.stack + p.bet
    return None


def _guess_raiser_seat(gs: GameState, my_seat: int) -> int | None:
    """Best-effort raiser seat when protocol didn't supply one (e.g. on reconnect).
    Prefers seats whose last_action was 'bet' and whose street_bet == current_bet."""
    # Prefer a seat whose last_action indicates a raise
    for s, p in gs.players.items():
        if s == my_seat or p.folded or p.busted:
            continue
        la = p.last_action if hasattr(p, 'last_action') else None
        if la and la.get('type') == 'bet' and p.street_bet == gs.current_bet:
            return s
    # Fallback: any live seat with street_bet matching current_bet
    for s, p in gs.players.items():
        if s != my_seat and not p.folded and p.street_bet == gs.current_bet:
            return s
    return None


# ---------------------------------------------------------------------------
# Postflop — equity + board-texture + ICM-aware
# ---------------------------------------------------------------------------

def _decide_postflop(gs: GameState, om: OpponentModel, my_seat: int, hole: list[str]) -> Decision:
    me = gs.players[my_seat]
    to_call = gs.current_bet - me.street_bet
    board = [c for c in gs.common if c]
    n_opp = max(1, len(gs.live_seats()) - 1)

    eq = equity.hand_equity_mc(hole, board, n_opp, samples=800 if n_opp > 2 else 1500)
    pot_odds = to_call / (gs.pot + to_call) if to_call > 0 else 0.0

    all_stacks = [p.stack + p.bet for p in gs.players.values() if not p.busted]
    avoid_variance = icm.should_avoid_marginal_spot(me.stack, all_stacks)

    bt = classify_board(board)

    # Adjust betting thresholds for board texture:
    # Wet boards → smaller bets (more draws that could beat us), lower threshold.
    # Dry boards → larger bets (fewer draws, our made hands are more protected).
    # Paired boards → check more with strong hands (deception + board advantage).
    if bt.is_wet:
        value_bet_threshold = 0.60   # lower bar to bet (semi-bluffs + made hands)
        bet_fraction_value = 0.55    # smaller sizing on wet boards
        bet_fraction_strong = 0.75
    elif bt.is_dry:
        value_bet_threshold = 0.65   # need real strength on dry boards
        bet_fraction_value = 0.75    # go bigger
        bet_fraction_strong = 1.00
    else:
        value_bet_threshold = 0.62
        bet_fraction_value = 0.66
        bet_fraction_strong = 1.00

    # Multi-way: tighten thresholds (our equity share shrinks)
    if n_opp >= 2:
        value_bet_threshold += 0.04  # need more equity to bet into many opponents
        pot_odds_margin = 0.08        # need more edge to call (pot odds + extra)
    else:
        pot_odds_margin = 0.10

    # Sizing on paired boards: often check/call traps rather than bet/fold
    check_trap = bt.paired and eq > 0.75 and not bt.trips_on_board

    # Check for an opponent who folds to c-bets a lot (exploit: bluff more)
    # Pick the most-likely opponent we're up against for profile lookup
    live = [s for s in gs.live_seats() if s != my_seat]
    bluff_bonus = 0.0
    if live:
        opp_seat = live[0]
        opp_profile = om.classify(opp_seat)
        if opp_profile == "fold_happy":
            bluff_bonus = 0.08   # add 8% equity to semi-bluffs vs. folders

    if to_call > 0:
        if to_call >= me.stack:
            # All-in call: use pot odds + variance guard
            threshold = pot_odds + (0.05 if avoid_variance else 0.0)
            if bt.is_wet:
                threshold -= 0.02  # wet board: equity may improve; slightly looser call
            return Decision("call_all") if eq >= threshold else Decision("fold")

        if eq >= pot_odds + pot_odds_margin:
            # Strong enough to raise?
            raise_threshold = 0.72 if not bt.is_wet else 0.76
            if eq > raise_threshold and not (avoid_variance and n_opp > 1) and not check_trap:
                # Sizing: on dry boards go bigger, on wet boards keep it reasonable
                raise_incr = max(gs.min_raise, int(gs.pot * (0.75 if bt.is_dry else 0.55)))
                return Decision("bet", raise_incr)
            return Decision("call")

        if eq >= pot_odds:
            return Decision("call")
        return Decision("fold")

    # Nothing to call: bet for value / semi-bluff, else check
    if check_trap:
        # Trap with strong hand on a paired board; check to encourage bluffs
        return Decision("check")

    effective_eq = eq + bluff_bonus
    if effective_eq > value_bet_threshold:
        size = int(gs.pot * (bet_fraction_strong if eq >= 0.80 else bet_fraction_value))
        min_bet = gs.min_raise if gs.current_bet else 1
        return Decision("bet", max(size, min_bet))

    # Semi-bluff on wet boards more, dry boards less
    semibluff_threshold = 0.42 if bt.is_wet else 0.47
    semibluff_freq = 0.35 if bt.is_wet else 0.20
    if (effective_eq > semibluff_threshold and random.random() < semibluff_freq
            and not avoid_variance and not bt.is_dry):
        size = int(gs.pot * 0.5)
        min_bet = gs.min_raise if gs.current_bet else 1
        return Decision("bet", max(size, min_bet))

    return Decision("check")
