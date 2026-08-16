from __future__ import annotations
import random
from . import ranges, icm, equity
from .state import GameState
from .opponent_model import OpponentModel


class Decision:
    def __init__(self, action: str, amount: int = 0):
        self.action = action  # 'fold' | 'check' | 'call' | 'call_all' | 'bet'
        self.amount = amount  # raise INCREMENT for 'bet' (see API.md)


def decide(gs: GameState, om: OpponentModel, my_seat: int, hole: list[str]) -> Decision:
    if gs.stage == "preflop":
        return _decide_preflop(gs, om, my_seat, hole)
    return _decide_postflop(gs, om, my_seat, hole)


# ---------------------------------------------------------------------------
# Preflop
# ---------------------------------------------------------------------------

def _decide_preflop(gs: GameState, om: OpponentModel, my_seat: int, hole: list[str]) -> Decision:
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

    # --- Short-stack push/fold zone (<=20bb effective): confirmed leaks live here ---
    if stack_bb <= 20:
        if is_allin_facing and to_call > 0:
            raiser_stack = _find_raiser_stack(gs, my_seat)
            risk = icm.icm_risk_premium(me.stack, raiser_stack, all_stacks) if raiser_stack else 0
            call_range = ranges.call_shove_range(my_pos, stack_bb, num_extra_risk=risk)
            if key in call_range:
                return Decision("call_all")
            return Decision("fold")

        if to_call == 0 or not facing_raise:
            push = ranges.push_range(my_pos, stack_bb)
            if key in push:
                return Decision("bet", me.stack)  # shove: amount caps at stack per API.md
            return Decision("check" if to_call == 0 else "fold")

        # facing a raise (not yet all-in for us) while short: treat as push/fold too
        push = ranges.push_range(my_pos, stack_bb)
        if key in push:
            return Decision("bet", me.stack)
        return Decision("fold")

    # --- Deep stack (>20bb): standard open / call / 3-bet logic -----------------
    if not facing_raise:
        opens = ranges.open_range(my_pos)
        if key in opens:
            size = int(2.5 * bb)
            return Decision("bet", size)
        return Decision("check" if to_call == 0 else "fold")

    # facing exactly one raise
    raiser_seat = _find_last_raiser_seat(gs, my_seat)
    raiser_pos_idx = ranges.POSITIONS_9MAX.index(pos_map.get(raiser_seat, "MP1")) \
        if raiser_seat in pos_map else 3
    my_pos_idx = ranges.POSITIONS_9MAX.index(my_pos) if my_pos in ranges.POSITIONS_9MAX else 3

    profile = om.classify(raiser_seat) if raiser_seat is not None else "unknown"
    value_3b = ranges.threebet_value_range(my_pos)
    bluff_3b_freq = 0.45 if profile in ("fold_happy", "nit") else 0.25

    if key in value_3b:
        size = int(3.2 * gs.current_bet) - gs.current_bet
        return Decision("bet", max(size, gs.min_raise))
    # thin, position-differentiated bluff 3-bets
    if my_pos in ("CO", "BTN", "SB") and random.random() < bluff_3b_freq:
        thin_bluffs = ranges.top_pct(3.0) - value_3b
        if key in thin_bluffs:
            size = int(3.2 * gs.current_bet) - gs.current_bet
            return Decision("bet", max(size, gs.min_raise))

    calls = ranges.call_open_range(my_pos, raiser_pos_idx, my_pos_idx)
    if key in calls:
        return Decision("call")
    return Decision("fold" if to_call > 0 else "check")


def _find_raiser_stack(gs: GameState, my_seat: int) -> int | None:
    for s, p in gs.players.items():
        if s != my_seat and not p.folded and p.street_bet == gs.current_bet:
            return p.stack + p.bet
    return None


def _find_last_raiser_seat(gs: GameState, my_seat: int) -> int | None:
    candidates = [s for s, p in gs.players.items()
                  if s != my_seat and not p.folded and p.street_bet == gs.current_bet]
    return candidates[0] if candidates else None


# ---------------------------------------------------------------------------
# Postflop -- intentionally simple v1 (equity + pot odds). See PLAN Phase 5
# for the board-texture / range-advantage engine to layer on top of this.
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

    if to_call > 0:
        if to_call >= me.stack:
            threshold = pot_odds + (0.05 if avoid_variance else 0.0)
            return Decision("call_all") if eq >= threshold else Decision("fold")
        if eq >= pot_odds + 0.10:
            if eq > 0.72 and not (avoid_variance and n_opp > 1):
                raise_to = gs.current_bet + max(gs.min_raise, int(gs.pot * 0.75))
                return Decision("bet", raise_to - gs.current_bet)
            return Decision("call")
        if eq >= pot_odds:
            return Decision("call")
        return Decision("fold")

    # nothing to call: bet for value / semi-bluff, else check
    if eq > 0.62:
        size = int(gs.pot * (0.66 if eq < 0.80 else 1.0))
        return Decision("bet", max(size, gs.min_raise if gs.current_bet else 1))
    if eq > 0.45 and random.random() < 0.30 and not avoid_variance:
        size = int(gs.pot * 0.5)
        return Decision("bet", max(size, gs.min_raise if gs.current_bet else 1))
    return Decision("check")
