"""
ICM layer.

pokerish's tournament mode is WINNER-TAKE-ALL (see API.md section 5: "Winner
takes all. When one player is left, tournament_over is broadcast with the
winner's full stack"). That simplifies the classic multi-payout ICM problem:
with only one payout, a player's tournament equity is just their probability
of finishing first, and under the standard (Malmuth-Harville-style) model
that probability is well approximated by their share of the total chips in
play:

    P(player i finishes 1st) ~= stack_i / sum(all stacks)

That's a first-order approximation (it ignores skill edge and future blind
pressure), but it gives two things a bot with no ICM logic structurally
cannot have:

  1. Correct recognition that chips are worth LESS marginally as your stack
     grows relative to the field (diminishing returns) -> big stacks should
     avoid marginal, high-variance all-ins they don't need.
  2. Correct recognition that short stacks must gamble sooner rather than
     bleed to antes -- pure survival value has no equity in a winner-take-all
     format, so (unlike multi-payout MTTs) there is no "min-cash" hesitance
     to build in. This makes Nemesis's short-stack shoving strategy simpler
     and more aggressive than a min-cash-aware bot would be, which is correct
     for THIS specific ruleset.
"""

from __future__ import annotations


def win_probability(my_stack: int, all_stacks: list[int]) -> float:
    total = sum(all_stacks)
    if total <= 0:
        return 0.0
    return my_stack / total


def icm_risk_premium(my_stack: int, opp_stack: int, all_stacks: list[int]) -> float:
    """Returns a 0..~3 'risk units' score used to tighten calling ranges
    (see ranges.call_shove_range's num_extra_risk). Higher when:
      - we're a big stack risking a lot of our equity share against a
        proportionally smaller threat (bad marginal trade in a winner-take-all
        format: busting a shorter stack barely changes our win probability
        much if we're already dominant, so it's not worth high variance)
      - we're already stack leader deep in the tournament (fewer players left
        raises the value of just not losing our equity share to variance)
    """
    total = sum(all_stacks)
    if total <= 0:
        return 0.0
    my_share = my_stack / total
    opp_share = opp_stack / total
    players_left = sum(1 for s in all_stacks if s > 0)

    premium = 0.0
    if my_share > 0.30 and opp_share < my_share * 0.5:
        # we're a big stack facing a much shorter stack -- avoid unnecessary variance
        premium += 1.5
    if players_left <= 4:
        premium += 1.0
    if players_left <= 2:
        premium -= 1.5  # heads-up: chip EV and tournament EV converge, stop over-tightening
    return max(0.0, premium)


def should_avoid_marginal_spot(my_stack: int, all_stacks: list[int],
                                stage: str = "river") -> bool:
    """Quick guard used outside of push/fold-table spots (e.g. deep-stack
    postflop marginal semi-bluff shoves) when we are a clear equity leader.

    Stage-aware: on the flop there are still two streets of cards to improve,
    so we can accept more variance (higher threshold required to block us).
    On the river the hand is final, so we protect our chip-equity lead more.
    This prevents the guard from over-folding profitable semi-bluffs early."""
    total = sum(all_stacks)
    if total <= 0:
        return False
    share = my_stack / total
    # Street-specific thresholds: more lenient early, stricter later.
    threshold = {
        "preflop": 0.45,  # need a dominant chip lead to tighten preflop
        "flop":    0.40,  # two streets left; accept more variance
        "turn":    0.37,  # one street left
        "river":   0.35,  # final decision; protect equity most aggressively
    }.get(stage, 0.35)
    return share > threshold
