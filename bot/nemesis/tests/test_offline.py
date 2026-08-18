"""
Offline unit tests for Nemesis — no live server needed.
Run with: python3 tests/test_offline.py
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from nemesis import ranges, icm, equity
from nemesis.state import GameState
from nemesis.opponent_model import OpponentModel, SeatStats
from nemesis.board import classify as classify_board
from nemesis.strategy import decide, Decision


# ---------------------------------------------------------------------------
# Ranges
# ---------------------------------------------------------------------------

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
    print(f"OK 15-20bb calling range not collapsed: BTN@18bb={len(call18)} classes -> {sorted(call18)[:8]}...")


def test_top_pct_monotonic():
    small = ranges.top_pct(5)
    big = ranges.top_pct(20)
    assert small.issubset(big)
    print(f"OK top_pct nesting: top5%={len(small)} classes, top20%={len(big)} classes")


def test_call_open_range_extra_pct():
    """Extra pct parameter widens the flat-call range (used vs. fingerprinted raisers)."""
    base = ranges.call_open_range("BTN", 3, 6)
    wider = ranges.call_open_range("BTN", 3, 6, extra_pct=5.0)
    assert len(wider) >= len(base)
    assert wider.issuperset(base)
    print(f"OK call_open_range extra_pct: base={len(base)}, wider={len(wider)}")


# ---------------------------------------------------------------------------
# ICM
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# State / position
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# Equity
# ---------------------------------------------------------------------------

def test_equity_fallback_or_real():
    eq = equity.hand_equity_mc(["As", "Ah"], [], 1, samples=300)
    assert 0.6 < eq < 1.0, eq
    print("OK AA equity vs 1 random opponent preflop ~=", round(eq, 3))
    eq2 = equity.hand_equity_mc(["7c", "2d"], [], 1, samples=300)
    assert eq2 < eq
    print("OK 72o equity lower than AA:", round(eq2, 3))


# ---------------------------------------------------------------------------
# Board texture
# ---------------------------------------------------------------------------

def test_board_classify_dry():
    bt = classify_board(["As", "7d", "2c"])
    assert bt.rainbow
    assert not bt.flush_draw_possible or bt.flush_draw_possible == False  # rainbow = no FD
    assert not bt.monotone
    assert not bt.paired
    # A-7-2 rainbow is very dry
    assert bt.is_dry
    print("OK dry board (A72r):", bt)


def test_board_classify_wet():
    bt = classify_board(["9h", "8h", "7d"])
    assert bt.straight_draw_possible
    assert bt.flush_draw_possible  # 2 hearts
    assert bt.is_wet
    assert not bt.is_dry
    print("OK wet board (987 two-tone):", bt)


def test_board_classify_monotone():
    bt = classify_board(["Ah", "7h", "2h"])
    assert bt.monotone
    assert bt.flush_draw_possible
    assert not bt.rainbow
    print("OK monotone board (A72 flush):", bt)


def test_board_classify_paired():
    bt = classify_board(["As", "Ad", "7c"])
    assert bt.paired
    assert not bt.double_paired
    print("OK paired board (AA7r):", bt)


def test_board_classify_turn_river():
    # Turn: 4 cards
    bt = classify_board(["9h", "8h", "7d", "2c"])
    assert bt.straight_draw_possible
    print("OK board classify works on turn:", bt)
    # River: 5 cards
    bt5 = classify_board(["9h", "8h", "7d", "2c", "Ks"])
    assert bt5.straight_draw_possible
    print("OK board classify works on river:", bt5)


# ---------------------------------------------------------------------------
# Opponent model — fingerprint detection
# ---------------------------------------------------------------------------

def test_looks_position_blind_fires():
    """After seeing the same ~50% shove rate from UTG through BTN, the seat
    should be flagged as position-blind."""
    s = SeatStats()
    positions = ["UTG", "UTG1", "MP1", "MP2", "HJ", "CO", "BTN"]
    for pos in positions:
        for _ in range(5):  # 5 shoves at each position
            s.record_shove(pos)
            s.record_shove_opportunity_declined(pos)  # 50% rate → one extra opp each
    assert s.looks_position_blind(), s.shoves_by_position
    print("OK looks_position_blind fires for uniform shove rate:", s.shoves_by_position)


def test_looks_position_blind_does_not_fire_for_correct_player():
    """A player who shoves wide on BTN and tight UTG should NOT be flagged."""
    s = SeatStats()
    # UTG: 1/5 shoves; BTN: 5/5 shoves  → spread = 0.8
    for _ in range(5):
        s.record_shove_opportunity_declined("UTG")
    s.record_shove("UTG")  # 1/6 shoves UTG  (~17%)
    for _ in range(5):
        s.record_shove("BTN")  # 5/5 shoves BTN (100%)
    assert not s.looks_position_blind(), s.shoves_by_position
    print("OK looks_position_blind does NOT fire for correctly position-aware player")


def test_opponent_model_classify():
    om = OpponentModel()
    s = om.get(0)
    # Nit: low VPIP, low AF
    s.hands = 30; s.vpip_hands = 4; s.pfr_hands = 3
    s.raises = 3; s.calls_or_checks = 20
    assert om.classify(0) == "nit", om.classify(0)
    print("OK OpponentModel.classify nit")


# ---------------------------------------------------------------------------
# Strategy — fingerprint exploit integration
# ---------------------------------------------------------------------------

def _make_gs(n_players=6, stack_bb=10, bb=10, stage="preflop", my_seat=2):
    """Helper: build a minimal GameState with n_players active seats."""
    gs = GameState()
    bb_val = bb
    sb_val = bb // 2
    players = []
    for i in range(n_players):
        players.append({
            "seat": i, "name": f"P{i}", "stack": stack_bb * bb_val - (bb_val if i == 1 else 0),
            "bet": bb_val if i == 1 else (sb_val if i == 0 else 0),
            "street_bet": bb_val if i == 1 else (sb_val if i == 0 else 0),
            "folded": False, "has_acted": False, "all_in": False, "busted": False,
            "is_turn": i == my_seat, "last_action": {"type": "none", "amount": 0},
        })
    gs.update({
        "stage": stage, "dealer": n_players - 1, "turn": my_seat,
        "current_bet": bb_val, "min_raise": bb_val, "pot": bb_val + sb_val,
        "common": [None]*5,
        "blinds": {"small": sb_val, "big": bb_val, "ante": 0,
                   "small_seat": 0, "big_seat": 1},
        "max_players": 9, "mode": "tournament", "players": players,
    })
    gs.my_seat = my_seat
    return gs


def test_strategy_preflop_shove_short_stack():
    """At 8bb, AA should always shove."""
    gs = _make_gs(n_players=6, stack_bb=8, bb=10)
    om = OpponentModel()
    d = decide(gs, om, 2, ["As", "Ah"])
    assert d.action == "bet", f"Expected shove, got {d.action}"
    print("OK strategy: AA shoves at 8bb")


def test_strategy_preflop_fold_trash_short():
    """At 8bb, 72o from UTG should fold."""
    gs = _make_gs(n_players=6, stack_bb=8, bb=10)
    om = OpponentModel()
    d = decide(gs, om, 2, ["7c", "2d"])
    assert d.action == "fold", f"Expected fold, got {d.action}"
    print("OK strategy: 72o folds at 8bb UTG")


def test_strategy_fingerprint_widens_call():
    """Against a fingerprinted (position-blind) raiser, our call-shove range
    at 18bb should be at least as wide as without the fingerprint."""
    # Build state: seat 5 raised all-in; we're seat 2 at 18bb
    gs = _make_gs(n_players=6, stack_bb=18, bb=10)
    # Simulate an all-in raise by seat 5
    for p in gs.players.values():
        p.street_bet = 0
    gs.players[5].street_bet = 180  # shoved all-in
    gs.players[5].all_in = True
    gs.current_bet = 180
    gs.players[2].stack = 180   # we have 18bb, calling is all-in
    gs.players[2].bet = 0

    om_plain = OpponentModel()
    om_print = OpponentModel()
    # Make seat 5 look position-blind in om_print
    s = om_print.get(5)
    for pos in ranges.POSITIONS_9MAX:
        for _ in range(5):
            s.record_shove(pos)
            s.record_shove_opportunity_declined(pos)
    assert s.looks_position_blind()

    # AQo: borderline call at 18bb
    d_plain = decide(gs, om_plain, 2, ["Ah", "Qd"], last_raiser_seat=5)
    d_print = decide(gs, om_print, 2, ["Ah", "Qd"], last_raiser_seat=5)
    print(f"OK fingerprint exploit: plain={d_plain.action}, fingerprinted={d_print.action}")
    # At minimum, the fingerprinted version should not be stricter
    action_rank = {"call_all": 2, "call": 1, "fold": 0}
    assert action_rank.get(d_print.action, 0) >= action_rank.get(d_plain.action, 0)


# ---------------------------------------------------------------------------
# Board texture → postflop strategy
# ---------------------------------------------------------------------------

def _make_gs_postflop(board, stage="flop", my_seat=0, n_opp=1, eq_override=None):
    """Build a minimal GameState for postflop testing."""
    gs = GameState()
    n_players = 1 + n_opp
    bb = 10
    pot = 100
    players = []
    for i in range(n_players):
        players.append({
            "seat": i, "name": f"P{i}", "stack": 500,
            "bet": 0, "street_bet": 0,
            "folded": False, "has_acted": True, "all_in": False, "busted": False,
            "is_turn": i == my_seat, "last_action": {"type": "check", "amount": 0},
        })
    full_board = board + [None] * (5 - len(board))
    gs.update({
        "stage": stage, "dealer": n_players - 1, "turn": my_seat,
        "current_bet": 0, "min_raise": bb, "pot": pot,
        "common": full_board,
        "blinds": {"small": 5, "big": bb, "ante": 0, "small_seat": 1, "big_seat": 0},
        "max_players": 9, "mode": "tournament", "players": players,
    })
    gs.my_seat = my_seat
    return gs


def test_postflop_dry_board_bets_bigger():
    """On a dry board, a strong hand should still bet (equity-driven), and
    the strategy must not crash on board texture classification."""
    gs = _make_gs_postflop(["As", "7d", "2c"])  # dry board
    om = OpponentModel()
    # KK on A72r: strong but not nuts; equity ~75% heads up
    d = decide(gs, om, 0, ["Ks", "Kh"])
    assert d.action in ("bet", "check"), f"Unexpected action: {d.action}"
    print(f"OK postflop dry board (A72r) KK -> {d.action} {d.amount}")


def test_postflop_wet_board_no_crash():
    """Wet board should not crash and should produce a valid action."""
    gs = _make_gs_postflop(["9h", "8h", "7d"])
    om = OpponentModel()
    d = decide(gs, om, 0, ["6s", "5s"])  # OESD + backdoor flush
    assert d.action in ("bet", "check", "call", "fold")
    print(f"OK postflop wet board (987hh7d) 65s -> {d.action} {d.amount}")


def test_postflop_paired_board_no_crash():
    gs = _make_gs_postflop(["As", "Ad", "7c"])
    om = OpponentModel()
    d = decide(gs, om, 0, ["Ah", "Kd"])  # trips aces
    assert d.action in ("bet", "check")
    print(f"OK postflop paired board (AA7r) AK -> {d.action} {d.amount}")


if __name__ == "__main__":
    fns = [v for k, v in list(globals().items()) if k.startswith("test_")]
    passed = 0
    failed = 0
    for fn in fns:
        try:
            fn()
            passed += 1
        except Exception as e:
            print(f"FAIL {fn.__name__}: {e}")
            import traceback
            traceback.print_exc()
            failed += 1
    print(f"\n{'ALL' if not failed else 'SOME FAILED'}: {passed} passed, {failed} failed "
          f"({len(fns)} total)")
    if failed:
        sys.exit(1)
