"""
Nemesis decision engine.

Preflop:
  - ≤20bb: position-aware Nash push/fold. Call ranges stay populated
    smoothly through the whole 1-20bb band, no dead zones.
    When a seat's observed shove pattern is fingerprinted as position-blind,
    we widen our call/re-shove range specifically vs. that seat.
  - >20bb: position-granular opens, 9-position 3-bet/call matrix.
    Bluff 3-bet frequency adjusted by opponent profile; against a
    position-blind-fingerprinted raiser, treat their range as ~15% looser.
  - BB iso-raise vs limpers: free EV against passive open-limpers.
  - 4-bet / stackoff response when we are the PFR facing a 3-bet.
  - Range-weighted pot-odds calls: equity computed against the opponent's
    estimated shove range rather than a random hand.

Postflop:
  - Monte Carlo equity via eval7 (falls back to heuristic if not installed).
  - C-bet logic: when we were the preflop raiser (is_pfr), bet more freely
    using range-advantage thresholds rather than pure equity thresholds.
  - Position-aware thresholds: bluff less OOP, raise more IP.
  - Board-texture-aware sizing: larger on dry boards, smaller on wet boards.
  - Multi-way pot thresholds tightened vs. single-opponent.
  - ICM chip-leader guard: stage-aware (lenient on flop, tighter on river).
  - Opponent profile adjustments: fold-happy opponents get more bluffs —
    aggregated across every live opponent, so a single calling station in
    the pot is enough to shut the exploit down.

None of this targets a specific named bot -- every adjustment above is
triggered by observed behavior (a fingerprint, a stat, a stack ratio), so it
adapts to whichever opponent is actually seated and does nothing extra
against an opponent that doesn't show the pattern.
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
           last_raiser_seat: int | None = None,
           i_am_pfr: bool = False) -> Decision:
    """
    Top-level decision function.

    `i_am_pfr` should be True if we made the last preflop raise/bet this hand,
    so that postflop c-bet logic and 3-bet-pot detection work correctly.
    """
    if gs.stage == "preflop":
        return _decide_preflop(gs, om, my_seat, hole, last_raiser_seat, i_am_pfr)
    return _decide_postflop(gs, om, my_seat, hole, is_pfr=i_am_pfr)


# ---------------------------------------------------------------------------
# Preflop helper: estimate what the opponent would shove with
# ---------------------------------------------------------------------------

def _estimate_shove_range(gs: GameState, om: OpponentModel,
                           opp_seat: int | None) -> set[str]:
    """Estimate the hands an opponent would shove with, combining:
      - Their Nash push range (position + effective stack)
      - Profile adjustment (nit shoves tighter; fish/aggro shoves wider)
      - Fingerprint override (position-blind player uses BTN range from any seat)

    This is Nemesis's key advantage over Titan and Prometheus: both of those
    compute equity vs a completely *random* hand (assuming the opponent shoves
    100% of hands). That systematically underestimates equity against tight
    players and overestimates it against loose ones. Modelling the actual
    distribution fixes both simultaneously.
    """
    if opp_seat is None or opp_seat not in gs.players:
        return ranges.top_pct(50.0)   # no info: assume wide

    pos_map = gs.position_map()
    opp_pos = pos_map.get(opp_seat, "MP1")
    opp_stack_bb = gs.effective_stack_bb(opp_seat)

    # Start from Nash push range at their position and effective stack depth
    base = ranges.push_range(opp_pos, opp_stack_bb)

    # Profile adjustments: observed behaviour overrides Nash assumptions
    profile = om.classify(opp_seat)
    if profile == "nit":
        # Nits shove tighter than Nash → model them as 40% deeper stacked
        # (a deeper-stack Nash range is narrower)
        base = ranges.push_range(opp_pos, min(20.0, opp_stack_bb * 1.4))
    elif profile in ("fish", "aggro"):
        # Loose/aggressive: shove wider than Nash → model as 35% shallower
        base = ranges.push_range(opp_pos, max(1.0, opp_stack_bb * 0.65))
    # fold_happy / reg / unknown: use Nash as-is

    # Fingerprint override: a position-blind player (characteristic of Prometheus)
    # shoves the BTN range from *any* seat. From UTG at 12bb that's ~35% vs Nash's
    # ~17% → their range is twice as wide, our equity vs it is much higher.
    if om.get(opp_seat).looks_position_blind():
        base = ranges.push_range("BTN", opp_stack_bb)

    return base if base else ranges.top_pct(50.0)


# ---------------------------------------------------------------------------
# Preflop
# ---------------------------------------------------------------------------

def _decide_preflop(gs: GameState, om: OpponentModel, my_seat: int, hole: list[str],
                    last_raiser_seat: int | None, i_am_pfr: bool = False) -> Decision:
    key = ranges.hand_key(hole[0], hole[1])
    pos_map = gs.position_map()
    my_pos = pos_map.get(my_seat, "MP1")
    me = gs.players[my_seat]
    bb = gs.big_blind()
    stack_bb = gs.effective_stack_bb(my_seat)

    to_call = gs.current_bet - me.street_bet
    all_stacks = [p.stack + p.bet for p in gs.players.values() if not p.busted]

    # Bug #5 fix: facing_raise requires an actual outstanding bet above the big
    # blind AND that we still owe chips (to_call > 0). Without the to_call guard,
    # the BB having posted looks like a "raise" to our own code when no one has
    # actually raised.
    facing_raise = (
        gs.current_bet > bb
        and to_call > 0
        and any(p.street_bet == gs.current_bet and p.seat != my_seat
                for p in gs.players.values())
    )
    is_allin_facing = to_call >= me.stack  # calling would commit our whole stack

    # 3-bet pot: we were the last aggressor preflop (i_am_pfr) and now face
    # a re-raise.  We need to decide between 4-bet/shove, call, or fold.
    is_3bet_pot = i_am_pfr and facing_raise

    # Whether the raiser shows Prometheus's position-blind fingerprint
    raiser_is_fingerprinted = (
        last_raiser_seat is not None
        and om.get(last_raiser_seat).looks_position_blind()
    )

    # --- Short-stack push/fold zone (≤20bb effective) ---
    if stack_bb <= 20:
        if is_allin_facing and to_call > 0:
            # ── Range-weighted pot-odds call (Nemesis's primary advantage) ──
            #
            # Titan / Prometheus: equity vs a random hand, call if in top-X%.
            # Problems:
            #   (a) ignores actual bet size (a 10x shove needs far more equity)
            #   (b) assumes opponent shoves 100% of hands (never true)
            #
            # Nemesis: compute real pot odds from the bet, then compute equity
            # *specifically against the opponent's estimated push range*.  This
            # corrects both errors simultaneously.

            # Step 1: real pot odds
            pot_total = gs.pot + to_call  # total in the pot after we call
            pot_odds = to_call / pot_total if pot_total > 0 else 0.5

            # Step 2: estimate opponent's push range and compute equity vs it
            opp_range = _estimate_shove_range(gs, om, last_raiser_seat)
            eq = equity.hand_equity_vs_range(hole, [], opp_range)
            if eq is None:
                # Fallback: random-hand MC (eval7 missing or range too tight)
                eq = equity.hand_equity_mc(hole, [], 1, samples=2000)

            # Step 3: ICM margin — how much extra equity above pot-odds we need
            # to account for tournament-life risk (chip EV ≠ $ EV).
            raiser_stack = _find_raiser_stack(gs, last_raiser_seat, my_seat)
            if raiser_stack:
                risk = icm.icm_risk_premium(me.stack, raiser_stack, all_stacks)
                # Convert 0..3 ICM risk units to an equity points margin
                icm_margin = risk * 0.025
            else:
                icm_margin = 0.04  # default when we don't know raiser's stack

            # Heads-up: chip-EV ≈ $ EV, so minimal ICM penalty
            if len(gs.live_seats()) == 2:
                icm_margin = max(0.0, icm_margin - 0.03)

            # Fingerprint exploit: raiser is position-blind → their range is
            # already modelled as wider in _estimate_shove_range, so our equity
            # is naturally higher. Reduce the margin slightly to call even wider.
            if raiser_is_fingerprinted:
                icm_margin = max(0.0, icm_margin - 0.015)

            if eq >= pot_odds + icm_margin:
                return Decision("call_all")
            return Decision("fold")

        if to_call == 0 or not facing_raise:
            push = ranges.push_range(my_pos, stack_bb)
            if key in push:
                return Decision("bet", me.stack)  # shove
            return Decision("check" if to_call == 0 else "fold")

        # Facing a raise (not yet all-in for us) while short: treat as push/fold
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

    # Detect BB vs limpers: we posted the big blind, nobody raised (current_bet
    # still equals the BB), and at least one other player has called in.
    is_bb = my_pos == "BB"
    in_bb_with_limpers = (
        is_bb and to_call == 0 and gs.current_bet == bb
        and any(
            s != my_seat and not p.folded and not p.busted and p.street_bet == bb
            for s, p in gs.players.items()
        )
    )
    if in_bb_with_limpers:
        limper_count = sum(
            1 for s, p in gs.players.items()
            if s != my_seat and not p.folded and not p.busted and p.street_bet == bb
        )
        iso_range = ranges.bb_iso_raise_range(limper_count)
        if key in iso_range:
            size = int((2.5 + limper_count) * bb)
            return Decision("bet", max(size, gs.min_raise))
        return Decision("check")

    if not facing_raise:
        opens = ranges.open_range(my_pos)
        if key in opens:
            size = int(2.5 * bb)
            return Decision("bet", size)
        return Decision("check" if to_call == 0 else "fold")

    # --- Facing a re-raise while we were already the aggressor (3-bet pot) ---
    # We must decide: 4-bet jam (value/bluff), flat-call, or fold.
    if is_3bet_pot:
        if key in ranges.STACKOFF_VS_3BET:
            return Decision("bet", me.stack)  # 4-bet shove for value
        if key in ranges.call_3bet_range(stack_bb):
            return Decision("call")
        # Blocker 4-bet bluffs (A5s, A4s, A3s) at 40% frequency
        if key in ranges.FOURBET_BLUFF_HANDS and random.random() < 0.40:
            return Decision("bet", me.stack)
        return Decision("fold" if to_call > 0 else "check")

    # --- Facing exactly one open raise: build full 9-position raiser context ---
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
    # Boost further if the raiser is fingerprinted as position-blind (wide range,
    # easily dominated by our 3-bet range)
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
# Postflop — equity + board-texture + position + ICM-aware
# ---------------------------------------------------------------------------

def _decide_postflop(gs: GameState, om: OpponentModel, my_seat: int, hole: list[str],
                     is_pfr: bool = False) -> Decision:
    me = gs.players[my_seat]
    to_call = gs.current_bet - me.street_bet
    board = [c for c in gs.common if c]
    n_opp = max(1, len(gs.live_seats()) - 1)
    stage = gs.stage  # "flop", "turn", or "river"

    # Position: are we last to act this street?  num_to_act_behind returns 0 when
    # no one acts after us, meaning we are in position.
    in_position = gs.num_to_act_behind(my_seat) == 0

    eq = equity.hand_equity_mc(hole, board, n_opp, samples=800 if n_opp > 2 else 1500)
    pot_odds = to_call / (gs.pot + to_call) if to_call > 0 else 0.0

    all_stacks = [p.stack + p.bet for p in gs.players.values() if not p.busted]
    # Stage-aware ICM guard: we need more equity to risk chips on later streets where
    # fewer runout cards remain to improve our hand.
    avoid_variance = icm.should_avoid_marginal_spot(me.stack, all_stacks, stage=stage)

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

    # OOP: need slightly more equity to bet into unclosed action
    if not in_position:
        value_bet_threshold += 0.02

    # Multi-way: tighten thresholds (our equity share shrinks)
    if n_opp >= 2:
        value_bet_threshold += 0.04  # need more equity to bet into many opponents
        pot_odds_margin = 0.08        # need more edge to call
    else:
        pot_odds_margin = 0.10

    # Sizing on paired boards: often check/call traps rather than bet/fold
    check_trap = bt.paired and eq > 0.75 and not bt.trips_on_board

    # Check for an opponent who folds to c-bets a lot (exploit: bluff more).
    # Aggregate across ALL live opponents rather than just the first live
    # seat found — in a multi-way pot, picking one seat arbitrarily meant a
    # read on a random opponent could override the read on whoever's
    # actually most relevant to the bluff/value decision.
    live = [s for s in gs.live_seats() if s != my_seat]
    profiles = [om.classify(s) for s in live] if live else []
    # A bluff only works if it's likely to get through everyone still in
    # the hand, so only boost bluff frequency if every live opponent with
    # a read looks fold-happy.
    modeled_profiles = [p for p in profiles if p != "unknown"]
    all_fold_happy = bool(modeled_profiles) and all(p == "fold_happy" for p in modeled_profiles)
    any_calling_station = any(p == "fish" for p in profiles)
    bluff_bonus = 0.0
    if all_fold_happy and not any_calling_station:
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
            # Raising OOP is harder to follow through on future streets; need more equity
            if not in_position:
                raise_threshold += 0.04
            if eq > raise_threshold and not (avoid_variance and n_opp > 1) and not check_trap:
                # Sizing: on dry boards go bigger, on wet boards keep it reasonable
                raise_incr = max(gs.min_raise, int(gs.pot * (0.75 if bt.is_dry else 0.55)))
                return Decision("bet", raise_incr)
            return Decision("call")

        if eq >= pot_odds:
            return Decision("call")
        return Decision("fold")

    # --- Nothing to call: bet for value / c-bet / semi-bluff, else check ---

    if check_trap:
        # Trap with strong hand on a paired board; check to encourage bluffs
        return Decision("check")

    effective_eq = eq + bluff_bonus

    # C-bet: when we were the preflop raiser we have a range advantage on most
    # boards (uncapped vs. capped caller ranges), so we can bet with a lower equity
    # threshold than pure value betting.  This is a significant EV source that
    # pure equity-based bots (like Prometheus's version) underexploit because they
    # don't track who was the preflop aggressor.
    if is_pfr:
        cbet_threshold = 0.38 if bt.is_dry else (0.52 if bt.is_wet else 0.45)
        if n_opp >= 2:
            cbet_threshold += 0.07  # c-bet much less multiway (range advantage shrinks)
        if not in_position:
            cbet_threshold += 0.04  # harder to c-bet OOP without position protection
        if effective_eq > cbet_threshold:
            size = int(gs.pot * (bet_fraction_strong if eq >= 0.75 else bet_fraction_value))
            min_bet = gs.min_raise if gs.current_bet else 1
            return Decision("bet", max(size, min_bet))
        # Pure bluff c-bet on dry boards (range/blocker advantage, high fold equity)
        if bt.is_dry and n_opp == 1 and not avoid_variance and random.random() < 0.22:
            size = int(gs.pot * 0.40)
            min_bet = gs.min_raise if gs.current_bet else 1
            return Decision("bet", max(size, min_bet))

    # Pure value bet (non-c-bet): only bet when we have clear equity advantage
    if effective_eq > value_bet_threshold:
        size = int(gs.pot * (bet_fraction_strong if eq >= 0.80 else bet_fraction_value))
        min_bet = gs.min_raise if gs.current_bet else 1
        return Decision("bet", max(size, min_bet))

    # Semi-bluff on wet boards more, dry boards less, and less OOP
    semibluff_threshold = 0.42 if bt.is_wet else 0.47
    semibluff_freq = 0.35 if bt.is_wet else 0.20
    if not in_position:
        semibluff_freq *= 0.65  # fewer bluffs OOP: less fold equity, more reverse-implied odds
    if (effective_eq > semibluff_threshold and random.random() < semibluff_freq
            and not avoid_variance and not bt.is_dry):
        size = int(gs.pot * 0.5)
        min_bet = gs.min_raise if gs.current_bet else 1
        return Decision("bet", max(size, min_bet))

    return Decision("check")
