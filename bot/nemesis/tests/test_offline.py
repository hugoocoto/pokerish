import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from nemesis import ranges, icm, equity
from nemesis.state import GameState


def test_canonical_order_basic():
    assert ranges.CANONICAL_ORDER[0] == "AA"
    assert "72o" in ranges.CANONICAL_ORDER
    assert len(set(ranges.CANONICAL_ORDER)) == 169
    print("OK canonical order:", ranges.CANONICAL_ORDER[:10], "...", ranges.CANONICAL_ORDER[-5:])


def test_hand_key():
    assert ranges.hand_key("As", "Ad") == "AA"
    assert ranges.hand_key("As", "Ks") == "AKs"
    assert ranges.hand_key("Ks", "Ah") == "AKo"
    print("OK hand_key")


def test_push_ranges_widen_correctly():
    # position ordering: shove range should widen UTG -> BTN at fixed depth
    utg = ranges.push_range("UTG", 10)
    btn = ranges.push_range("BTN", 10)
    assert len(btn) > len(utg), (len(utg), len(btn))
    print(f"OK position widening at 10bb: UTG={len(utg)} classes, BTN={len(btn)} classes")

    # THE Prometheus-specific check: at 18bb (its dead zone), Nemesis must
    # still shove/call a nonzero range instead of collapsing to premiums only
    call18 = ranges.call_shove_range("BTN", 18)
    assert len(call18) >= 5, call18
    print(f"OK 15-20bb calling range not collapsed: BTN@18bb call range size={len(call18)} classes -> {sorted(call18)[:8]}...")


def test_top_pct_monotonic():
    small = ranges.top_pct(5)
    big = ranges.top_pct(20)
    assert small.issubset(big)
    print(f"OK top_pct nesting: top5%={len(small)} classes, top20%={len(big)} classes")


def test_icm_win_probability():
    stacks = [3000, 3000, 3000, 1000]
    p = icm.win_probability(3000, stacks)
    assert abs(p - 0.30) < 1e-9
    print("OK icm.win_probability:", p)


def test_icm_risk_premium_bigstack_vs_shortstack():
    stacks = [6000, 1000, 1000, 1000, 1000]
    premium = icm.icm_risk_premium(6000, 1000, stacks)
    assert premium > 0
    print("OK icm_risk_premium (big stack vs short, should tighten):", premium)


def test_state_position_map_full_ring():
    gs = GameState()
    players = [{"seat": i, "name": f"P{i}", "stack": 1000, "bet": 0, "street_bet": 0,
                "folded": False, "has_acted": False, "all_in": False, "busted": False,
                "is_turn": False} for i in range(9)]
    gs.update({
        "stage": "preflop", "dealer": 0, "turn": 3, "current_bet": 20, "min_raise": 20,
        "pot": 30, "common": [None]*5,
        "blinds": {"small": 10, "big": 20, "ante": 0, "small_seat": 1, "big_seat": 2},
        "max_players": 9, "mode": "tournament", "players": players,
    })
    pm = gs.position_map()
    assert pm[2] == "BB", pm
    assert pm[1] == "SB", pm
    assert pm[0] == "BTN", pm
    assert pm[3] == "UTG", pm
    print("OK 9-max position_map:", pm)


def test_state_position_map_with_busted_seats():
    gs = GameState()
    seats = [0, 2, 3, 6, 8]  # some seats busted/empty
    players = [{"seat": i, "name": f"P{i}", "stack": 1000, "bet": 0, "street_bet": 0,
                "folded": False, "has_acted": False, "all_in": False,
                "busted": (i not in seats), "is_turn": False} for i in range(9)]
    gs.update({
        "stage": "preflop", "dealer": 0, "turn": 2, "current_bet": 20, "min_raise": 20,
        "pot": 30, "common": [None]*5,
        "blinds": {"small": 2, "big": 3, "ante": 0, "small_seat": 2, "big_seat": 3},
        "max_players": 9, "mode": "tournament", "players": players,
    })
    pm = gs.position_map()
    assert pm[3] == "BB"
    assert pm[2] == "SB"
    assert pm[0] == "BTN"
    print("OK position_map with empty seats (5-handed):", pm)


def test_equity_fallback_or_real():
    eq = equity.hand_equity_mc(["As", "Ah"], [], 1, samples=300)
    assert 0.6 < eq < 1.0, eq
    print("OK AA equity vs 1 random opponent preflop ~=", round(eq, 3))
    eq2 = equity.hand_equity_mc(["7c", "2d"], [], 1, samples=300)
    assert eq2 < eq
    print("OK 72o equity lower than AA:", round(eq2, 3))


if __name__ == "__main__":
    fns = [v for k, v in list(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
    print("\nALL", len(fns), "OFFLINE TESTS PASSED")
